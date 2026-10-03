# Network Protocol Audit (Warriors and Wizards, reference source)

Audit phase only. Nothing in the reference source was changed, built or run. Every claim was checked against the code. Paths are relative
to `REF` = `Reference - This Is The Source Being Ported To Unity/Warriors-and-Wizards-Testing`. `file:N` means line N of that file at audit time
(2026-10-02). Where the code and the docs (`README.md`, `CLAUDE.md`, `Docs/EngineeringAudit.md`) disagree, both are given in section 10.
**UNVERIFIED** marks anything not fully confirmed from the code.

Short names used below:

| Short | Path |
|---|---|
| `SRV` | `WaW-Server/GameServer` |
| `CMN` | `WaW-Server/Common` |
| `CLI` | `WaW-Client/WaWClient/Networking` |
| `PROTO` | `Shared/Common.Protocol` |
| `WEB` | `WebClient` |

---

## 0. Summary

- **One TCP connection per player**, game port **2050**. Each packet is `[int32 LE total length, counting the 5 header bytes][byte packet id][body]`.
  Everything is **little-endian**. There is no compression, no encryption (no TLS on the desktop game socket), no sequence numbers,
  no checksums and no protocol-level keep-alive.
- **One packet-id enum** (`PROTO/PacketId.cs`, `byte`) is shared by both clients and the server. It has 79 named ids (0-79, 77 retired)
  plus `Unknown = 255`. Of those, **41 are live on the wire**: 20 client->server (C->S) and 21 server->client (S->C). The rest are dead
  code, retired, or "not implemented on either side".
- **The packet bodies are written by hand twice**: once in the client's `Write`/`Read` and once in the server's `Read`/`Write`.
  No schema or code generator is used. Wire tests exist only for the id values (`Shared/Tests/Common.Protocol.Tests/PacketIdTests.cs`)
  and two Python E2E scripts (`Tools/E2E`).
- **Server tick rate is 20 TPS** (50 ms). Every tick the server sends each player one `NewTick` (stat/position deltas of visible
  entities). It sends an `Update` only when tiles or entities appear or disappear. The client replies to every `NewTick` with one
  `Move` (its own position). **Movement is client-authoritative** with a server plausibility check (snap-back by `Goto`).
  **Hits are also client-reported** (`EnemyHit`/`PlayerHit`). The server runs its own collision too and checks every reported hit
  for plausibility.
- **World switches happen on the same connection.** The server sends `Reconnect(gameId)`; the client re-sends `Hello` and then
  `Load`/`Create` on the same socket. The credentials are re-sent too, but the server skips re-verifying them.
- **The GameServer and AccountServer talk over JSON-RPC** (StreamJsonRpc 2.25.29), over TLS with a pinned certificate plus a
  shared-secret handshake, on port 8081 (loopback).
- **Live wire mismatches found** (section 10): `AccountList` (layout), `Notification`/`ShowEffect` colour byte order, `ShowEffect` layout,
  the stat enum above id 82, and the client's string-stat list (122-125). Non-fatal `Failure`s are treated as fatal by the client.

---

## 1. Transport and framing

### 1.1 Transport

| Item | Value | Source |
|---|---|---|
| Socket | TCP/IPv4, `Socket(AddressFamily.InterNetwork, Stream, Tcp)`, bound to `IPAddress.Any` | `SRV/Game/Network/SocketServer.cs:29-30` |
| Port | 2050 (`<Port>`) | `CMN/Resources/Config/Data/gameServerConfig.xml:5`; client `WaW-Client/WaWClient/Core/Settings.cs:50` |
| Listen backlog | 3000 | `SocketServer.cs:31` |
| Nagle | disabled on both ends (`NoDelay = true`) | server `SRV/Game/Network/NetworkHandler.cs:62`; client `CLI/Client.cs:77` |
| Max players (pre-allocated `User` objects) | `MaxPlayers` = 1000; when all are in use the socket is closed | `SocketServer.cs:27,70-79`; `gameServerConfig.xml:9` |
| Per-address cap | `MaxClientsPerIP` = 10. **127.0.0.1 / ::1 is never capped**, so all browser players (who arrive through the loopback bridge) are uncapped | `SocketServer.cs:60-68`; `SRV/Game/Network/ConnectionLedger.cs:29`; `gameServerConfig.xml:14` |
| Client connect retry | desktop: up to 10 attempts, 1 s apart, only on `ConnectionRefused`. Web: retries forever, 1.5 s apart | `CLI/Client.cs:61,81-96`; `WEB/web/shim/WebClient.cs:61-74` |
| TLS / encryption | **none** on the game socket (desktop). Browser: WSS to nginx, then plain TCP on the VPS loopback | section 1.6 |
| Compression | none | (no compression code in any reader or writer) |

### 1.2 Frame layout

```
offset 0  int32  LE   Length  = total frame size INCLUDING these 4 bytes and the id byte (so body length = Length - 5)
offset 4  byte        PacketId (PROTO/PacketId.cs)
offset 5  ...         Body (Length - 5 bytes), packet-specific, no padding, no alignment
```

- Writer (server): `CMN/Network/SocketSendState.cs:45-74`. The body is written at `start+5`, then
  `totalLen = writer.Position - start` and `Write(int totalLen)`, `Write(byte id)` go at `start`. Client writer:
  `CLI/SocketSendState.cs:46-79` (the same scheme). Web writer: `WEB/web/shim/WebClient.cs:169-181`.
- Reader: `CMN/Network/SocketReceiveState.cs:46-79` (server) and `CLI/SocketReceiveState.cs:46-79` (client). Both peek the int32 and
  require `5 <= Length <= buffer size`. Anything else throws `InvalidDataException`, which disconnects the peer
  (server `NetworkHandler.cs:140-143`, client `CLI/Client.cs:163-169`). They wait until `Length` bytes are present, then hand a
  `SpanReader` over exactly the body slice (`span.Slice(5, Length-5)`), so **a body mis-parse can never desync the stream**. The
  next frame always starts at the declared length.
- **Body over-read**: reading past the body throws. The server catches it, logs "Error handling message", drops that packet and keeps
  the connection (`NetworkHandler.cs:157-159`). The client does the same (`CLI/Client.cs:178-180`).
- **Body under-read**: trailing bytes are silently ignored on both sides.
- **Unknown id**: the server drops it silently. There is no factory entry, `TryGetValue` is false, and nothing is logged
  (`NetworkHandler.cs:151`). On the client, `PacketUtils.CreateIncomingPacket` throws `ArgumentException("Unsupported packet ID")`,
  which is logged and dropped (`CLI/Packets/Packet.cs:99`, `CLI/Client.cs:178-180`).
- No header field for sequence, ack, timestamp, flags or version exists.

### 1.3 Size limits and buffering

| Side | Buffer | Limit | Source |
|---|---|---|---|
| Server receive | `ArrayPool.Rent(0x20000)` = 128 KiB, compacted on each receive | **Max inbound frame 131 072 bytes**; a larger length disconnects | `NetworkHandler.cs:41`, `CMN/Network/SocketReceiveState.cs:14-17,31-39,55` |
| Server send | two 512 KiB buffers (`0x80000`), write buffer and send buffer swapped; grows on overflow | no hard cap. **Resize bug**: `ResizeBuffer(ref _writeBuffer, _writeLength)` rents `_writeLength*2`, which can be *smaller* than the current buffer (or 0 when empty), and `WritePacket` then recurses. A packet that overflows a buffer less than half full recurses without end (stack overflow). Found by reading the code; never seen at runtime | `CMN/Network/SocketSendState.cs:22-25,58-61,76-82` |
| Server send diag | packets > 10 000 bytes are logged at Debug | | `SocketSendState.cs:66-67` |
| Client receive (desktop) | 256 KiB (`0x40000`) | **Max inbound frame 262 144 bytes** | `CLI/SocketReceiveState.cs:14,50` |
| Client send (desktop) | 64 KiB start, doubles up to **1 MiB**; a packet that does not fit even then is **dropped and counted**, not thrown | | `CLI/SocketSendState.cs:15-16,46-91`; `CLI/Client.cs:230-246` |
| Client receive (web) | 256 KiB start, `Array.Resize` when appended data exceeds it; the frame check `length > _recv.Length` runs before growth, so the effective limit is also ~256 KiB | | `WEB/web/shim/WebClient.cs:41,107-121` |
| Client send (web) | fixed 64 KiB scratch, no growth; a bigger packet throws out of `QueuePacket` | | `WebClient.cs:40,169-181` |
| Packets per drain (server) | log at 200, **disconnect at > 2000 packets per drain** ("Packet flood", `IllegalAction`) | | `NetworkHandler.cs:170-171,194-199` |
| Strings | `ushort` length prefix, so 65 535 bytes UTF-8 max per string. `WriteUTF` does not check the length (it would truncate the prefix silently) | | `CMN/Network/SpanWriter.cs:164-179` |

### 1.4 Flush cadence

- **Server**: the game loop calls `HandleIncomingPackets()` and `SendSocketData()` for every user on **every spin of the loop**, not
  only per tick (`SRV/Game/GameLogic.cs:235-243`). The loop sleeps about 1 ms between spins until the tick is due
  (`GameLogic.cs:45-58`). So outgoing packets leave within about 1 ms of being written. All packets written during one tick
  (`Update`, `NewTick`, ...) normally leave in **one `SendAsync`** (double-buffer swap, one send in flight: `SocketSendState.cs:84-109`).
- **Client (desktop)**: `Client.Tick()` (once per client frame) first flushes the queued outgoing bytes (`SendPendingPackets`), then
  handles all received packets (`CLI/Client.cs:186-208`). Received packets are parsed on a thread-pool receive loop and queued
  (`Client.cs:114-184`), then applied on the main thread.
- **Client (web)**: the same order. Each queued packet is sent as **its own WebSocket binary message** (`WebClient.cs:152-167`).

### 1.5 Threading and ordering guarantees

- TCP gives in-order, reliable delivery. **Every packet is "reliable, ordered"**, and there is only one channel.
- Server: incoming packets go into a per-user `ConcurrentQueue`, drained on the game thread. **If a handler `await`s** (Hello, Load,
  Create, BuyStorage, ClaimStarter, ...), that user's later packets wait until it completes. Other users are not blocked
  (`NetworkHandler.cs:165-223`). Many handlers do not mutate the world directly. They `GameLogic.Enqueue(...)` the work, which runs
  on the next drain (`GameLogic.cs:210-216`), so the effect of a packet is applied in the next loop iteration, still in arrival order.
- Server world ticks run in `Parallel.ForEach` over worlds (`GameLogic.cs:246-259`). `SendPacket` from those ticks writes into the
  user's unlocked `SocketSendState`. This is safe only because a user is in exactly one world (UNVERIFIED for every path).

### 1.6 WebSocket path (browser client)

```
browser (WASM)  --wss://<domain>/game-->  nginx :443  --ws://127.0.0.1:2051-->  ws_bridge.py  --TCP 127.0.0.1:2050-->  GameServer
```

- `WEB/web/shim/WebClient.cs` replaces `CLI/Client.cs` wholesale in the web build (`WEB/web/patched.props:10`). The header comment says
  it carries "the exact same packet byte stream ... `[int32 length][byte id][body]`, just carried in WebSocket binary frames of any
  size" (`WebClient.cs:1-3`). The receive side reassembles frames into a byte stream exactly like TCP (`WebClient.cs:107-140`).
- Bridge: `WEB/vps/ws_bridge.py` (the same file as `WEB/tools/ws_bridge.py`). It is a byte pipe with no framing awareness: client->server
  messages are written raw, and server bytes are read in 64 KiB chunks and sent as binary messages (`ws_bridge.py:12-34`). It is
  started as `ws_bridge.py 2051 127.0.0.1 2050` by systemd unit `ww-bridge` (`WEB/vps/setup_web.sh:29`). The `websockets` server
  is created with `max_size=None, ping_interval=20` (`ws_bridge.py:39`). **That WebSocket ping is the only keep-alive anywhere in
  the game path.**
- nginx: `location /game { proxy_pass http://127.0.0.1:2051; ... proxy_read_timeout 3600s; }` (`setup_web.sh:80-87`). The game URL
  comes from JS `getGameUrl` (`WEB/web/WebHost.cs:20,26`).
- Effects of this path: every browser player shows up as `127.0.0.1` on the game server. The per-IP cap and the IP in logs are
  meaningless for them (`ConnectionLedger.cs:29`). Text messages from the browser would be UTF-8-encoded by the bridge
  (`ws_bridge.py:19`); the client only sends binary.

---

## 2. Primitive encoding

Both sides use structurally identical `SpanReader`/`SpanWriter` ref structs (`CMN/Network/SpanReader.cs`,
`CMN/Network/SpanWriter.cs`, `CLI/SpanReader.cs`, `CLI/SpanWriter.cs`). A `diff` shows only formatting and namespace changes, plus
`Write(WorldPosData)` (server) vs `Write(Position)` (client), which produce the same bytes. The constructor default is
`littleEndian = true` ("Little endian is for network, big endian is for maps", `SpanReader.cs:16-17`). **No packet uses big-endian.**

| Primitive | Read / Write method | Bytes | Layout |
|---|---|---|---|
| bool | `ReadBoolean` / `Write(bool)` | 1 | `0x00` false, `0x01` true; any non-zero reads as true (`SpanReader.cs:24-28`) |
| byte | `ReadByte` / `Write(byte)` | 1 | raw |
| sbyte | (no reader) / generic array path only | 1 | (casts to `short` overload via implicit conversion, UNVERIFIED, unused) |
| int16 | `ReadInt16` / `Write(short)` | 2 | LE two's complement |
| uint16 | `ReadUInt16` / `Write(ushort)` | 2 | LE |
| int32 | `ReadInt32` / `Write(int)` | 4 | LE two's complement |
| uint32 | `ReadUInt32` / `Write(uint)` | 4 | LE |
| int64 / uint64 | `ReadInt64` / `ReadUInt64` / `Write(long/ulong)` | 8 | LE (not used by any live packet) |
| float32 | `ReadSingle` / `Write(float)` | 4 | IEEE-754 binary32 LE. NaN is used as a sentinel in projectile paths |
| float64 | `ReadDouble` / `Write(double)` | 8 | IEEE-754 LE (not used by any live packet) |
| UTF string | `ReadUTF` / `WriteUTF(ReadOnlySpan<char>)` | 2+n | `uint16 LE byteCount` + UTF-8 bytes, **no terminator**. `null` and `""` both write `00 00` (`SpanWriter.cs:164-179`). Max 65 535 bytes |
| Long UTF string | `Read32UTF` / `Write32UTF` | 4+n | `int32 LE byteCount` + UTF-8. Used only for `Hello.MapJSON` (the server reads it manually: `SRV/Game/Session/Hello.cs:41-42`) |
| C string | `ReadNullTerminatedString` / `WriteNullTerminatedString` | n+1 | UTF-8 + `0x00` (not used by any live packet) |
| raw bytes | `ReadBytes(n)` | n | no prefix |
| generic array `T[]` | `Write<T>(T[])` (writer only) | 2+... | `uint16 LE count` + each element with its scalar writer. `string[]`: count + `WriteUTF` each. `char[]`: **byte count** + UTF-8. `byte[]`: count + raw (`SpanWriter.cs:55-99`). There is no matching generic reader. Used by `AccountList` (int[]) only |
| WorldPosData / Position | `WorldPosDataIO.Read` / `Write(WorldPosData)` | 8 | `float32 X`, `float32 Y` (`CMN/Network/WorldPosDataIO.cs:6-8`, `SpanWriter.cs:181-184`, `CLI/Structs/DataObjects/Position.cs:245-253`) |
| EntityId / object id | `EntityId.Read` / `Write(id.Value)` | 4 | int32 LE. Server-side the value packs `index = Value & 0xFFFFF`, `generation = (Value >> 20) & 0xFFF` (`CMN/Utilities/Collections/EntityId.cs:10-12,22-23`). The client treats it as an opaque int |
| colour | `Write(int)` (server) / `ARGB.Read` (client) | 4 | **mismatch, see section 10 D3**: the server writes an int32 LE `0x00RRGGBB` (bytes `BB GG RR 00`); the client reads bytes in order as A,R,G,B (`CLI/Structs/DataObjects/BGRA.cs`) |

**There is no compressed or variable-length integer anywhere.** No 7-bit/varint encoding exists in either reader or writer. The
legacy `NetworkReader`/`NetworkWriter` (`BinaryReader`/`BinaryWriter` subclasses) are used only to read map files, big-endian
(`CMN/Resources/World/MapData.cs:215`). The client's copy (`CLI/NetworkParsing.cs`) is not used by the protocol. Its `WriteUTF`
writes **no length prefix** (`NetworkParsing.cs:236-245`), which would be a bug if it were ever used.

Count prefixes used by packets are not uniform: `int16`, `uint16`, `byte` and `int32` all appear. The table in section 5 gives each one.

---

## 3. Connection and session state machine

### 3.1 States

Server `ConnectionState` (`SRV/Game/Network/User.cs:22-27`): `Disconnected` (pooled, free) -> `Connected` -> (`Reconnecting`) -> `Ready`.
Server `GameState` (`SRV/Game/Network/GameInfo.cs:13-17`): `Idle` -> `Loading` (after Hello) -> `Playing` (after Load/Create).
Client `ConnectionState` (`CLI/Client.cs:19-22`): `Disconnected` / `Connected`, plus the `Client.IsReconnecting` flag.

```
 client                                   server
 TCP connect ------------------------->   accept; Ledger.TryAdd(ip) (cap) ; Pop pooled User (full -> close)       SocketServer.cs:49-88
                                          RealmManager.UserConnected: N x ServerProjectileProps (BEFORE Hello)   RealmManager.cs:51-56,93-104
                                          State = Connected, start receive                                        User.cs:66-69
 Hello(build, gameId, user, pw, "") -->   version == config.Version ? else Failure(1, serverVersion) + close     Hello.cs:46-49
                                          [not Reconnecting] RPC VerifyAccount; null -> Failure(0,...);
                                          AccountInUse -> Failure(4)                                                Hello.cs:52-67
                                          banned (RPC GetActiveBans) -> Failure(0,"Account is banned.")            Hello.cs:74-88
                                          AdminOnly && !admin -> Failure(0)                                         Hello.cs:90-93
                                          resolve gameId (EntryWorlds) ; -2 test -> Failure(2,"Not available")      Hello.cs:98-133
                                          SetGameInfo (GameState=Loading), RPC GetMuteState                         Hello.cs:135-139
                 <-------------------     MapInfo ; WeatherState                                                    Hello.cs:141-152
 (MapInfo.Handle: Map.InitMap)
 Create(class,skin,role) or Load(charId) ->                                                                         CLI MapInfo.cs:58,66-82
                                          Load: [not Reconnecting] RPC GetCharacter; dead/banned/world deleted
                                                -> Failure(0); roleless -> Failure(0); maybe Reroute (Reconnect)    Load.cs:24-63, CeremonyGate.cs:19-41
                                          Create: RPC CreateCharacter; null -> Failure(0, status); maybe Reroute    Create.cs:25-49
                                          User.Load (queued to game thread): State=Ready, GameState=Playing,
                                          player entity spawned                                                     User.cs:78-98, GameInfo.cs:63-80
                 <-------------------     CreateSuccess(objectId, charId); AccountList(0, locked);
                                          AccountList(1, ignored); [new player in Nexus +1.5s] StarterOffer;
                                          [Campsite] StorageInfo                                                    User.cs:84-96
                 <---- every tick ----    Update (only when something new) + NewTick (always)                      PlayerSightManager.cs:49-61
 Move(pos) per NewTick ---------------->  plausibility -> accept or Goto + AntiCheat                               Move.cs:19-60
 ...play...
```

### 3.2 Handshake and version check

- `Hello` must be the first packet in practice. The server **does not enforce** this: packets other than Hello are processed in any
  state. Most check `GameState == Playing`; a few (e.g. `InvSwap`, `InvDrop`, `CatAction`) do not check the state at the packet
  layer (see the validation notes in section 5).
- Version: exact string equality `Hello.BuildVersion != GameServerConfig.Config.Version` (`Hello.cs:46`). Both are currently
  `"0.3.16"` (`gameServerConfig.xml:10`, `WaW-Client/WaWClient/Core/Settings.cs:30`).
- **On a version mismatch** the server sends `Failure(ErrorId=1 INCORRECT_VERSION, ErrorDescription=<server version>)`, flushes it,
  then disconnects (`User.cs:111-120`). The client records the server version **in `Read`** on the network thread, because the
  disconnect can clear the queue before `Handle` runs (`CLI/Packets/Incoming/Failure.cs:24`). `Handle` repeats this and disconnects
  (`Failure.cs:33,36`). Every client disconnect also re-fetches `/app/version` from the AccountServer (`CLI/Client.cs:265-267`).
  The client also checks the version up front over HTTP `/app/version` (`WaW-Client/WaWClient/AppEngine/VersionCheck.cs`, server
  `WaW-Server/AccountServer/Systems/App/Version.cs`), using the same exact-match rule.
- **Before `Hello` is even read**, the server pushes every custom `ServerProjectileProps` on accept (`RealmManager.cs:53`), so an
  outdated client still receives them.

### 3.3 Authentication

- The credentials are the **plain username and password in `Hello`**, re-sent on every world switch (`CLI/Packets/Incoming/Reconnect.cs:33-41`).
  There is no session token. Desktop clients send them in clear text over TCP (`Docs/EngineeringAudit.md` F30 says the same).
- Verified by RPC `VerifyAccount(username, password, serverGuid)`, which also takes the account lock for this server
  (AccountServer side `AccServerRpcHandler.cs:42-45`; lock details belong to the AccountServer audit).

### 3.4 Load / Create

- The client decides by itself which to send when `MapInfo` arrives. If `GlobalData.CharacterType > 0` it sends `Create`, otherwise
  `Load(SelectedCharacterId)` (`CLI/Packets/Incoming/MapInfo.cs:66-82`).
- The server replies with `CreateSuccess(objectId, charId)`. The client stores `Map.LocalPlayerId` and `SelectedCharacterId`
  (`CLI/Packets/Incoming/CreateSuccess.cs`).

### 3.5 Map info

`MapInfo` is sent once per Hello (`Hello.cs:141-151`). The client resets the map, inits it, then sends Load/Create
(`CLI/Packets/Incoming/MapInfo.cs:48-58`).

### 3.6 Reconnect / world switch (same socket)

1. The server decides on a switch: `UsePortal`, `Escape`, walking onto a region (`RegionTriggers`), or `CeremonyGate.Reroute`.
   `User.ReconnectTo(world)` sets `State = Reconnecting`, unloads the player (saves the character, leaves the world, `GameState = Idle`)
   and sends `Reconnect(world.Id)` (`User.cs:122-131`). `CeremonyGate.Reroute` does the same without the unload, because the player
   was never loaded (`CeremonyGate.cs:29-31`).
2. **The connection stays open.** The client fades to black, wipes the world, then queues a new `Hello` with `GameId = reconnect id`
   and the same credentials (`CLI/Packets/Incoming/Reconnect.cs:19-41`).
3. The server, seeing `State == Reconnecting`, **skips `VerifyAccount`** and reuses `GameInfo.Account` (`Hello.cs:52`). It resolves
   the world **from the id the client sent**, not from the id it handed out (`Hello.cs:98`; see section 9.2 U9), then sends `MapInfo`.
4. The client sends `Load`. With `Reconnecting`, the server **skips `GetCharacter` and ignores `CharId`**: it uses `GameInfo.Char`
   (`Load.cs:30-39`). Then `User.Load` runs and sends `CreateSuccess` again.

The E2E script follows a Reconnect the same way, on the same connection (`Tools/E2E/e2e_roles.py:133`).

### 3.7 Escape

`Escape` (no body). If the player is playing and not blocked by the role gate and not already in the Nexus, the server records the
run stats and calls `ReconnectTo(Nexus)` (`SRV/Game/Session/Escape.cs:13-26`). In the Nexus the player gets the chat text
"You're already in the Nexus!".

### 3.8 Disconnect

- There is **no disconnect packet**. The server closes the socket (`SocketServer.DisconnectUser`, `SocketServer.cs:94-108`) after
  `Failure`, after `Death` (+1500 ms, `SRV/Game/Systems/Combat/PlayerDeath.cs:20,70-72`), on a flood, on the anti-cheat kick, and
  on shutdown (`Failure(0,"The server is restarting...")`, `GameLogic.cs:125`).
- `User.Disconnect` sets the state, then queues `FinishDisconnect` on the game thread: unload/save, remove from `RealmManager`,
  close the socket, and return the `User` to the pool (`User.cs:133-150`).
- Client: `Client.Disconnect` closes the socket, clears the queue, resets the map, re-fetches the version and goes back to the
  character list (`CLI/Client.cs:248-269`).

### 3.9 Failure codes (`SRV/Game/Session/Failure.cs:8-13`)

| Id | Name | Sent when | Disconnect? |
|---|---|---|---|
| 0 | `DEFAULT` | most refusals (bad credentials, banned, invalid world, character dead, roleless, create failed, kicked, anti-cheat kick, shutdown) | yes |
| 1 | `INCORRECT_VERSION` | Hello version mismatch; the description is the server's version | yes |
| 2 | `FORCE_CLOSE_GAME` | Hello with GameId -2 (test world) | yes |
| 3 | `INVALID_TELEPORT_TARGET` | **never sent** (no caller found) | - |
| 4 | `ACCOUNT_IN_USE` | Verify returned AccountInUse | yes |
| 5 | `PORTAL_DISABLED` | `UsePortal` / region refusal ("Invalid world.", "World is deleted.", "Portal disabled.", "You are not in a guild.") | **server: no** (`disconnect:false`, `UsePortal.cs:28-33`, `RegionTriggers.cs:38,64`). **Client: yes**, because `Failure.Handle` always calls `Client.Disconnect` (`CLI/Packets/Incoming/Failure.cs:36`). See section 10 D5 |

The client gives special treatment only to id 1 (`VersionCheck.IncorrectVersionFailureId = 1`, `VersionCheck.cs:21`).

### 3.10 Keep-alive, ping and timeouts

- **There is no game-level ping or keep-alive.** `Ping`/`Pong` classes exist on the client, but both use `PacketId.Unknown`. `Ping` is
  commented out of the factory (`CLI/Packets/Packet.cs:85`), and `Pong` would be dropped by `QueuePacket` (`CLI/Client.cs:231`).
  The server has neither.
- **There is no server idle or login timeout.** A socket that connects and never sends Hello holds a `User` slot until TCP fails.
  The only limits are the per-IP cap (not applied to loopback/browser) and `MaxPlayers` (`SocketServer.cs:60-79`). No TCP keep-alive
  option is set. In practice the server sends `NewTick` every 50 ms to *loaded* players, so a dead peer is detected through send
  errors (`NetworkHandler.cs:81-99`).
- Timeouts that do exist: `GotoAckTimeoutMs = 3000` (`SRV/Game/Systems/Combat/PlausibilityRules.cs:25`), 10 s waits inside
  ClaimStarter and BuyStorage (`ClaimStarter.cs:27`, `StorageShop.cs:55,78`), the RPC connect timeout of 5 s
  (`SRV/Program.cs:47,98`), and the WebSocket bridge ping of 20 s (`ws_bridge.py:39`).

---

## 4. Complete packet table

Direction: C->S = client to server, S->C = server to client. Status values:
**live** = written by the sender and read by the receiver in shipping code paths.
**dead** = a class exists on at most one side, or is never sent/handled.
**n/i** = the enum comment says "not implemented on either side" and no class exists.
**retired** = the number must never be reused.

The client and server enums **cannot disagree**: there is one shared enum (`PROTO/PacketId.cs`), aliased by
`global using PacketId = Common.Structs.PacketId;` in `CLI/Packets/PacketIds.cs:4` and `SRV/Game/Network/Messaging/PacketId.cs:3`.
The mismatches that do exist are in client classes that use `PacketId.Unknown` instead of their enum value (marked below).

| Id | Name | Dir | Status | Server class (`file`) | Client class | Notes |
|---|---|---|---|---|---|---|
| 0 | Failure | S->C | live | `SRV/Game/Session/Failure.cs` | `Incoming/Failure.cs` | client always disconnects |
| 1 | CreateSuccess | S->C | live | `Session/CreateSuccess.cs` | `Incoming/CreateSuccess.cs` | |
| 2 | Create | C->S | live | `Session/Create.cs` | `Outgoing/Create.cs` | ushort written, int16 read |
| 3 | PlayerShoot | C->S | live | `Systems/Projectiles/PlayerShoot.cs` | `Outgoing/PlayerShoot.cs` | one packet = one bullet |
| 4 | Move | C->S | live | `Network/Messaging/Move.cs` | `Outgoing/Move.cs` | one per NewTick |
| 5 | PlayerText | C->S | live | `Systems/Chat/PlayerText.cs` | `Outgoing/PlayerText.cs` | |
| 6 | Text | S->C | live | `Systems/Chat/Text.cs` | `Incoming/Text.cs` | |
| 7 | ServerPlayerShoot | S->C | live | `Systems/Projectiles/PlayerShoot.cs:106` | `Incoming/ServerPlayerShoot.cs` | other players' bullets |
| 8 | Damage | S->C | dead | none | `Incoming/Damage.cs` (empty Handle) | server never sends |
| 9 | Update | S->C | live | `Network/Messaging/Update.cs` | `Incoming/Update.cs` | |
| 10 | Notification | S->C | live | `Network/Messaging/Notification.cs` | `Incoming/Notification.cs` | colour byte order mismatch (D3) |
| 11 | NewTick | S->C | live | `Network/Messaging/NewTick.cs` | `Incoming/NewTick.cs` | |
| 12 | InvSwap | C->S | live | `Systems/Inventory/InvSwap.cs` | `Outgoing/InvSwap.cs` | |
| 13 | UseItem | C->S | live | `Systems/Inventory/UseItem.cs` | `Outgoing/UseItem.cs` | |
| 14 | ShowEffect | S->C | live on the wire, **dead on the client** | `Network/Messaging/ShowEffect.cs` | `Incoming/ShowEffect.cs` (empty Handle) | layout mismatch (D4) |
| 15 | Hello | C->S | live | `Session/Hello.cs` | `Outgoing/Hello.cs` | |
| 16 | Goto | S->C | live | `Network/Messaging/Goto.cs` | `Incoming/Goto.cs` | anti-cheat snap-back |
| 17 | InvDrop | C->S | live | `Systems/Inventory/InvDrop.cs` | `Outgoing/InvDrop.cs` | |
| 18 | InvResult | S->C | live | `Systems/Inventory/InvResult.cs` | `Incoming/InvResult.cs` (Handle is `//todo`) | |
| 19 | Reconnect | S->C | live | `Session/Reconnect.cs` | `Incoming/Reconnect.cs` | same socket |
| 20 | MapInfo | S->C | live | `Session/MapInfo.cs` | `Incoming/MapInfo.cs` | |
| 21 | Load | C->S | live | `Session/Load.cs` | `Outgoing/Load.cs` | |
| 22 | Teleport | C->S | dead | none | `Outgoing/Teleport.cs` (never sent) | |
| 23 | UsePortal | C->S | live | `Systems/Portals/UsePortal.cs` | `Outgoing/UsePortal.cs` | |
| 24 | Death | S->C | live | `Network/Messaging/Death.cs` | `Incoming/Death.cs` | |
| 25 | Buy | C->S | dead | none | `Outgoing/Buy.cs` (never sent) | |
| 26 | BuyResult | S->C | dead | none | `Incoming/BuyResult.cs` (empty Handle) | |
| 27 | Aoe | S->C | dead | none | `Incoming/Aoe.cs` (empty Handle) | |
| 28 | PlayerHit | C->S | live | `Systems/Combat/PlayerHit.cs` | `Outgoing/PlayerHit.cs` | |
| 29 | EnemyHit | C->S | live | `Systems/Combat/EnemyHit.cs` | `Outgoing/EnemyHit.cs` | |
| 30 | AoeAck | C->S | dead | none | `Outgoing/AoeAck.cs` (never sent) | |
| 31 | ShootAck | - | dead | none | none | |
| 32 | SquareHit | - | dead | none | none | |
| 33 | EditAccountList | C->S | dead | none | `Outgoing/EditAccountList.cs` uses **`PacketId.Unknown`** (`:8`), so it is dropped by `QueuePacket` even though `PartyData.cs:75-114` sends it | id/class mismatch |
| 34 | AccountList | S->C | live | `Session/AccountList.cs` | `Incoming/AccountList.cs` | **layout mismatch (D2)** |
| 35 | DamageCounterUpdate | - | n/i | | | |
| 36 | CreateGuild | C->S | dead | none | `Outgoing/CreateGuild.cs` (never sent) | |
| 37 | GuildResult | S->C | dead | none | `Incoming/GuildResult.cs` | |
| 38 | GuildRemove | C->S | dead | none | `Outgoing/GuildRemove.cs` | |
| 39 | GuildInvite | C->S | dead | none | `Outgoing/GuildInvite.cs` | |
| 40 | EnemyShoot | S->C | live | `Systems/Combat/EnemyShoot.cs` | `Incoming/EnemyShoot.cs` | |
| 41 | Escape | C->S | live | `Session/Escape.cs` | `Outgoing/Escape.cs` | |
| 42 | InvitedToGuild | S->C | dead | none | `Incoming/InvitedToGuild.cs` | |
| 43 | JoinGuild | C->S | dead | none | `Outgoing/JoinGuild.cs` | |
| 44 | ChangeGuildRank | C->S | dead | none | `Outgoing/ChangeGuildRank.cs` | |
| 45 | PlaySound | S->C | dead | none | `Incoming/PlaySound.cs` (empty Handle) | |
| 46 | Reskin | C->S | dead | none | `Outgoing/Reskin.cs` | replaced by ChangeSkin (74) |
| 47 | GotoAck | C->S | live | `Network/Messaging/Goto.cs:18` | `Outgoing/GotoAck.cs` | |
| 48 | ServerProjectileProps | S->C | live | `Systems/Projectiles/ServerProjectileProps.cs` | `Incoming/ServerProjectileProps.cs` | sent at accept |
| 49 | ConstellationsSave | - | n/i | | | |
| 50 | OptionsChanged | - | dead (no class found on either side) | | | |
| 51-54 | StatsApply, StatsApplyResult, GemstoneApply, GemstoneRemove | - | n/i | | | |
| 55 | TradeRequest | C->S | dead | none | `Outgoing/RequestTrade.cs` uses **`PacketId.Unknown`** (`:6`) | id/class mismatch |
| 56 | TradeRequested | S->C | dead | none | `Incoming/TradeRequested.cs` | |
| 57 | TradeStart | S->C | dead | none | `Incoming/TradeStart.cs` | `TradeItem` differs (D9) |
| 58 | ChangeTrade | C->S | dead | none | `Outgoing/ChangeTrade.cs` | |
| 59 | TradeChanged | S->C | dead | none | `Incoming/TradeChanged.cs` | |
| 60 | CancelTrade | C->S | dead | none | `Outgoing/CancelTrade.cs` | |
| 61 | TradeDone | S->C | dead | none | `Incoming/TradeDone.cs` | |
| 62 | AcceptTrade | C->S | dead | none | `Outgoing/AcceptTrade.cs` | |
| 63 | TradeAccepted | S->C | dead | none | `Incoming/TradeAccepted.cs` | |
| 64 | GemstoneSwap | - | n/i | | | |
| 65 | PartyInvite | - | n/i | | | |
| 66 | ChooseRole | - | retired 2026-09-27 | | | |
| 67 | RoleResult | - | retired 2026-09-27 | | | |
| 68 | StarterOffer | S->C | live | `Session/ClaimStarter.cs:102` | `Incoming/StarterOffer.cs` | |
| 69 | ClaimStarter | C->S | live | `Session/ClaimStarter.cs:17` | `Outgoing/ClaimStarter.cs` | |
| 70 | StarterResult | S->C | live | `Session/ClaimStarter.cs:114` | `Incoming/StarterResult.cs` | |
| 71 | BuyStorage | C->S | live | `Session/StorageShop.cs:19` | `Outgoing/BuyStorage.cs` | |
| 72 | StorageInfo | S->C | live | `Session/StorageShop.cs:135` | `Incoming/StorageInfo.cs` | |
| 73 | WeatherState | S->C | live | `Systems/Weather/WeatherControl.cs:55` | `Incoming/WeatherState.cs` | |
| 74 | ChangeSkin | C->S | live | `Session/SkinChange.cs:17` | `Outgoing/ChangeSkin.cs` | |
| 75 | UsePowerUp | C->S | live | `Systems/Inventory/PowerUps.cs:216` | `Outgoing/UsePowerUp.cs` | |
| 76 | CatAction | C->S | live | `Systems/Inventory/CatCare.cs:153` | `Outgoing/CatAction.cs` | |
| 77 | (JumpStart) | - | retired 2026-10-01, no enum member | | | `PacketId.cs:84` |
| 78 | ClaimBounty | C->S | live | `Systems/Rewards/BountyBoard.cs:119` | `Outgoing/ClaimBounty.cs` | |
| 79 | QuestObjId | S->C | live | `Systems/Quests/Quests.cs:58` | `Incoming/QuestObjId.cs` | |
| 255 | Unknown | - | marker, never sent | | | |

Client packet classes that have **no packet id** (`PacketId.Unknown`) and so never reach the wire: incoming `AllyShoot`, `ClientStat`,
`File`, `GlobalNotification`, `NameResult`, `Pic`, `Ping` (all commented out of the factory, `CLI/Packets/Packet.cs:61-85`); outgoing
`ChooseName`, `EditAccountList`, `OtherHit`, `Pong`, `RequestTrade`, `UpdateAck`. Note that **`UpdateAck` is "sent" after every
`Update`** (`Incoming/Update.cs:66`) and silently dropped (`CLI/Client.cs:231`).

Server registration is by reflection: `[Packet(PacketId.X)]` on each `IIncomingPacket`, collected by `PacketLib.LoadIncoming`
(`SRV/Game/Network/Messaging/Packet.cs:20-33`). The server handles exactly the 20 C->S ids marked live above.

---

## 5. Packet details (all live packets)

Notation: fields are listed in wire order. `W` = writer location, `R` = reader location. Client paths are under `CLI/Packets/`.
Unless noted otherwise, every packet is **reliable and ordered** (TCP, single channel).

### 5.1 Session

#### Hello (15, C->S)
- **Purpose**: login, version check, and pick the target world. It is also the second half of every world switch.
- **Fields**: `BuildVersion: UTF`, `GameId: int32`, `Username: UTF`, `Password: UTF`, `MapJSON: int32 len + UTF-8 bytes` (always `""`, so `00 00 00 00`).
  W `Outgoing/Hello.cs:23-27`; R `SRV/Game/Session/Hello.cs:36-43` (manual `ReadInt32` + `ReadBytes`, which matches `Write32UTF`). **Match.**
- **GameId values**: `WorldIds.Nexus = -1`, `Test = -2`, `Campsite = -5`, `GuildHall = -6`, `Tutorial = -7` (`PROTO/WorldIds.cs:7-11`); any
  positive value is a live world id (from a `Reconnect`).
- **Validation**: exact version (`Hello.cs:46`); credentials and lock via RPC unless `Reconnecting` (`:52-67`); ban (`:74-88`); AdminOnly
  (`:90-93`); world resolution and `AllowsEntry` (`EntryWorlds.cs:13-34`); test world refused (`Hello.cs:104-106`).
- **Server behaviour**: random 31-bit seed (`:135`), `SetGameInfo` (creates `ClientRandom` from the seed and a separate server random,
  `User.cs:71-76`), mute state via RPC, then `MapInfo` + `WeatherState`.
- **Client behaviour**: sent at connect (`CLI/Client.cs:271-280`) and after `Reconnect` (`Incoming/Reconnect.cs:33-41`).
- **Related**: AccountServer `VerifyAccount`, `GetActiveBans`, `FlushAccount`, `GetMuteState`.

#### MapInfo (20, S->C)
- **Fields**: `Width: int32`, `Height: int32`, `Name: UTF`, `DisplayName: UTF`, `Seed: uint32`, `Background: int32`, `ShowDisplays: bool`,
  `AllowPlayerTeleport: bool`, `Music: UTF`, `Difficulty: int32`. W `SRV/Game/Session/MapInfo.cs:19-30`; R `Incoming/MapInfo.cs` (Read). **Match**
  (the client comment notes it was wrong before 2026-09-21).
- **Client**: `Map.Reset`, `Map.InitMap(...)`, then sends `Create` or `Load`, starts the world music, clears `IsReconnecting`.
- **Note**: `Seed` is sent but no client-side use of it for RNG sync was checked (UNVERIFIED).

#### Load (21, C->S)
- **Fields**: `CharId: int32`. W `Outgoing/Load.cs`; R `Session/Load.cs:20-22`. **Match.**
- **Validation**: account banned (`:25-28`); character exists via RPC (`:31-39`, skipped when Reconnecting, and **CharId is then ignored**);
  dead (`:46-49`); world deleted (`:51-55`); roleless refused; ceremony reroute (`:60`).
- **Server**: `User.Load` queues the spawn and sends `CreateSuccess`, 2 x `AccountList`, maybe `StarterOffer` (delayed 1.5 s) and `StorageInfo`
  (`User.cs:78-98`).
- **Ordering dependency**: requires `Hello` first. Otherwise `GameInfo.Account` is null, the handler throws, and the server answers
  "Internal error handling packet" and disconnects (`NetworkHandler.cs:204-220`).

#### Create (2, C->S)
- **Fields**: `ClassType: uint16` (server reads `int16`), `SkinType: uint16` (server reads `int16`), `Role: UTF`. W `Outgoing/Create.cs`; R
  `Session/Create.cs:19-23`. Wire-compatible: the server casts back to `ushort` (`:26`).
- **Validation**: everything (class, skin ownership, role validity, slot count) is in RPC `CreateCharacter` on the AccountServer.
- **Server**: adopts the returned account, ceremony reroute (`CeremonyGate.Reroute(..., created:true)`), then `User.Load`.

#### CreateSuccess (1, S->C)
- **Fields**: `ObjectId: int32`, `CharId: int32`. W `Session/CreateSuccess.cs:10-13`; R `Incoming/CreateSuccess.cs`. **Match.**
- **Client**: `Map.LocalPlayerId = ObjectId`, `GlobalData.SelectedCharacterId = CharId`.

#### Failure (0, S->C)
- **Fields**: `ErrorId: int32`, `ErrorDescription: UTF`. W `Session/Failure.cs:17-20`; R `Incoming/Failure.cs` (Read). **Match.**
- **Server**: `SendFailure(id, msg, disconnect=true)` flushes the socket before disconnecting (`User.cs:111-120`).
- **Client**: records the version when id = 1 (in Read and Handle), then **always** `Client.Disconnect` (`Failure.cs:36`).

#### Reconnect (19, S->C)
- **Fields**: `GameId: int32`. W `Session/Reconnect.cs:9-11`; R `Incoming/Reconnect.cs`. **Match.**
- **Client**: `WorldLoad.Begin`, fade, `Map.ClearWorldObjects`, then a new `Hello` with that GameId on the same socket.

#### Escape (41, C->S)
- **Fields**: none. **Validation**: `GameState.Playing`, `CeremonyGate.Blocks`, not already in the Nexus (`Session/Escape.cs:14-23`).
- **Server**: `RunStats.Escape`, `ReconnectTo(Nexus)`.

#### AccountList (34, S->C)
- **Purpose**: the locked (0) and ignored (1) account-id lists, sent after every Load.
- **Server writes**: `AccountListId: int32`, then `SpanWriter.Write(int[])` = **`uint16 count` + `count x int32`** (`Session/AccountList.cs:11-14`, `SpanWriter.cs:55-99`).
- **Client reads**: `AccountListId: int32`, `count: int16`, then **`count x UTF` string, each `int.Parse`d** (`Incoming/AccountList.cs:19-22`).
- **MISMATCH (D2)**. Empty lists (count 0) parse fine. A non-empty list throws in `Read` (a bad UTF length or a `FormatException`),
  is logged, and the packet is dropped. Because of the framing, nothing else breaks.

#### Death (24, S->C)
- **Fields**: `AccountId: int32`, `CharId: int32`, `KilledBy: UTF`, `ClassType: uint16`, `Level: int32`, `BaseFame: int32`, `TotalFame: int32`,
  `statCount: byte`, then `statCount x (Label: UTF, Value: UTF)`, then `bonusCount: byte`, then `bonusCount x (Name: UTF, Description: UTF, Amount: int32)`.
  The server caps both counts at 255. W `SRV/Game/Network/Messaging/Death.cs:13-33`; R `Incoming/Death.cs` (Read). **Match.**
- **Server**: sends it, then disconnects 1500 ms later (`PlayerDeath.cs:20,62,70-72`).
- **Client**: builds a `DeathReport`, sets `DeathScreen.Active`, disconnects by itself (`Incoming/Death.cs:55`) and shows the death screen.

### 5.2 World state

#### Update (9, S->C)
- **Purpose**: newly seen tiles, newly visible entities (full stat snapshot), entities that left sight.
- **Fields**:
  ```
  tileCount: int16, tileCount x Tile { X: int16, Y: int16, GroundType: uint16 }                 (6 bytes each)
  newCount:  int16, newCount  x ObjectData { ObjectType: uint16, ObjectStatus }                    (see 5.6)
  dropCount: int16, dropCount x Drop { ObjectId: int32, Explode: bool }                            (5 bytes each)
  ```
  W `SRV/Game/Network/Messaging/Update.cs:25-35`, tiles `CMN/Resources/World/MapData.cs:141-145`, objects `CMN/Structs/ObjectData.cs:14-17`
  -> `ObjectStatusData.WriteForUpdate` (`CMN/Structs/ObjectStatusData.cs:32-47`), drops `CMN/Structs/ObjectDropData.cs:10-13`.
  R `Incoming/Update.cs:37-58` (`TileDef`, `ObjectDef`, drop `ReadInt32` + discarded `ReadBoolean`). **Match.**
- **Server**: sent from `PlayerSightManager.ProcessUpdate` **only when any list is non-empty** (`PlayerSightManager.cs:76-79`).
  `Explode` is always false (never set).
- **Client**: for each tile `Map.SetTileData`. For each new object it creates `Player` or `Entity` from `ObjectLibrary` (unknown types fall
  back to type `0x017e` as a placeholder), applies its stats and position, and detects the local player. For each drop it calls
  `Map.RemoveEntity`. Then it queues `UpdateAck`, which is dropped (`Incoming/Update.cs:65-113`).

#### NewTick (11, S->C)
- **Fields**: `count: int16`, then `count x ObjectStatus` (5.6), where the stat list is the **delta** (`StatUpdates`).
  W `Network/Messaging/NewTick.cs:29-37` -> `ObjectStatusData.WriteForNewTick` (`ObjectStatusData.cs:49-64`); R `Incoming/NewTick.cs` (Read, `ObjectStats.Read`). **Match.**
- **Server**: sent **every tick to every player with a sight component, even when empty** (`PlayerSightManager.cs:60,282-284`).
- **Client**: **first queues a `Move` with its own current position** (if the local player exists), then applies each status (stats + `OnTickPosition`)
  (`Incoming/NewTick.cs:42-48`). There is no tick id, timestamp or acknowledgement number.

#### Move (4, C->S)
- **Fields**: `X: float32`, `Y: float32` (the new position). W `Outgoing/Move.cs`; R `Network/Messaging/Move.cs:67-69`. **Match.**
- **Validation** (`Move.cs:19-60`):
  - ignored unless `GameState.Playing` (`:20`);
  - ignored while a `Goto` is outstanding and younger than 3000 ms (`:27-31`);
  - speed: `distance > MovementRules.MaxDistance(now - LastMoveAtMs)` leads to a snap-back plus `AntiCheat.Report(MovedTooFar)` (`:39-49`).
    `MaxDistance = min(elapsed, 2000) * 0.0096 * 1.5 * 1.5 + 1.0` tiles (`PlausibilityRules.cs:12-21`);
  - walls: `MovementRules.CanEnter` (another tile must be walkable, with no FullOccupy/OccupySquare object) leads to a snap-back plus
    `WalkedThroughWall` (`Move.cs:50-54`, `PlausibilityRules.cs:29-41`).
- **Server**: `player.Move(world, x, y)` (this sets `PositionUpdate`), then `RegionTriggers.OnMoved` (the guild-hall door or a realm-portal
  tile can trigger a `Reconnect`) (`Move.cs:56-59`, `RegionTriggers.cs:18-44`).
- **Client**: the position is integrated locally every frame. Only the position at `NewTick` time is reported, so the effective rate is
  20 Hz, one Move per NewTick.

#### Goto (16, S->C) / GotoAck (47, C->S)
- **Goto fields**: `ObjectId: int32`, `X: float32`, `Y: float32`. W `Network/Messaging/Goto.cs:12-15`; R `Incoming/Goto.cs`. **Match.**
- **GotoAck fields**: `Time: int32`. W `Outgoing/GotoAck.cs` (the client never sets `Time`, so it is always 0); R `Goto.cs:28-30`. The server
  ignores `Time`.
- **Flow**: the server snaps back (`Move.SnapBack`, `Move.cs:62-65`), sets `GotoSentMs`, and ignores Moves. The client teleports its player
  (only when `ObjectId == LocalPlayer.ObjectId`; `Map.LocalPlayer` is dereferenced without a null check) and answers `GotoAck`. The server
  then clears `GotoSentMs` and restarts the speed window (`Goto.cs:22-26`). Without an ack for 3 s, the server resumes taking Moves.

#### Notification (10, S->C)
- **Fields (server)**: `ObjectId: int32`, `Txt: UTF`, `Color: int32 (0xRRGGBB)`, `Size: int32` (default 24), `IsDamage: bool`.
  W `Network/Messaging/Notification.cs:10-16`.
- **Fields (client)**: `ObjectId: int32`, `Message: UTF`, `Color: 4 bytes read as A,R,G,B`, `Size: int32`, `IsDamage: bool`. R
  `Incoming/Notification.cs:36-41`.
- **Byte count matches, colour interpretation does not (D3)**. Example: `0x3CC46A` goes on the wire as `6A C4 3C 00`. The client reads A=0x6A,
  R=0xC4, G=0x3C, B=0x00, so `rgb = 0xC43C00` (`Notification.cs:50`).
- **Client**: floating text over the entity, with stagger. Damage numbers (`IsDamage`) are shown at once.

#### ShowEffect (14, S->C)
- **Server fields** (fixed 29 bytes): `EffectType: byte`, `TargetId: int32`, `Color: int32`, `EffectParam: float32`, `Pos1: 2 x float32`, `Pos2: 2 x float32`.
  W `Network/Messaging/ShowEffect.cs:16-23`. Sent by behaviours (`AOE.cs:165`, `HealGroup.cs:57,66`, `HealSelf.cs:76,85`).
- **Client fields**: `EffectType: byte`, then a **per-type variable layout** (e.g. Heal = target + colour; Teleport = pos1; ...)
  (`Incoming/ShowEffect.cs:23-110`). **MISMATCH (D4).** It is harmless today only because `Handle()` is empty (`ShowEffect.cs:112`).

#### QuestObjId (79, S->C)
- **Fields**: `ObjectId: int32` (0 = none). W `Systems/Quests/Quests.cs:61-63`; R `Incoming/QuestObjId.cs`. **Match.** Client sets `Map.QuestId`.
  The quest entity is also pushed beyond the sight radius by `PlayerSightManager.AddQuest` (`:261-280`).

#### WeatherState (73, S->C)
- **Fields**: `Weather: UTF` (auto|clear|cloudy|rain|storm), `Time: UTF` (auto|day|golden|dusk|night|dawn). W `WeatherControl.cs:58-61`;
  R `Incoming/WeatherState.cs`. **Match.** Sent after every Hello and broadcast on change (`WeatherControl.cs:40-48`).

### 5.3 Combat

#### PlayerShoot (3, C->S)
- **Fields**: `Angle: float32` (**radians**). W `Outgoing/PlayerShoot.cs`; R `Systems/Projectiles/PlayerShoot.cs:99-101`. **Match.**
- **Implicit bullet id**: the client numbers its bullets itself (`Player.GetBulletId`, sends one PlayerShoot per projectile of an attack,
  `WaW-Client/WaWClient/Game/Objects/Player.cs:490-505`). The server assigns the next local id **in packet order, including refused
  shots** (`EntityProjectiles.Skip`, `PlayerShoot.cs:58-66`). The two counters must stay in lock-step. Ids wrap at 2000
  (`EntityProjectiles.cs:14,21`; client `Projectile.WrapBulletId`). **This hidden coupling breaks if any PlayerShoot is lost or reordered**,
  which cannot happen on TCP but would on UDP.
- **Validation**: `State == Ready`, `Playing`, not `Dead` (`:26`). A weapon must exist (or the Tutorial stand-in, `:40-42,91-97`). Fire rate is a
  leaky bucket over `AttackPeriodMs(dex, rateOfFire)` with x1.3 slack and a capacity of 3 attacks (`:47-56`, `PlausibilityRules.cs:47-72`).
  A refusal adds anti-cheat points (`FiredTooFast`).
- **Server**: the server-side damage roll times attack (`:69`). **The spawn position is the server's own player position, not a client value**
  (`:70`). It spawns one bullet and sends `ServerPlayerShoot` to every other player within the sight radius (`:72-84`).

#### ServerPlayerShoot (7, S->C)
- **Fields**: `BulletId: uint16`, `OwnerId: int32`, `WeaponType: uint16`, `StartPos: 2 x float32`, `AngleDeg: float32` (**degrees**).
  W `PlayerShoot.cs:109-115`; R `Incoming/ServerPlayerShoot.cs`. **Match.** Client: a cosmetic bullet (no hit reports), shooter animation.

#### EnemyShoot (40, S->C)
- **Fields**: `FirstBulletId: uint16`, `OwnerId: int32`, `ProjId: byte`, `StartPos: 2 x float32`, `Angle: float32`, `Damage: int32`, `NumShots: byte`,
  `AngleInc: float32`, `Path: ProjectilePath`. W `Systems/Combat/EnemyShoot.cs:21-31`; R `Incoming/EnemyShoot.cs` (Read). **Match.**
- **ProjectilePath encoding** (W `CMN/Projectiles/ProjectilePaths/ProjectilePath.cs:54-60`, R `WaW-Client/WaWClient/Game/Objects/ProjectilePaths/ProjectilePath.cs:115-124`):
  ```
  segmentCount: int32
  segmentCount x { type: byte (PathType: 0 Line,1 Wavy,2 Circle,3 Amplitude,4 Boomerang,5 Accelerate,6 Decelerate,7 ChangeSpeed,8 Combined),
                   segment body }
  base segment body: Speed float32, LifetimeMs int32, FixedAngle float32 (NaN = use shot angle), TimeOffset int32, Mods int32 (bit 1<<1 = Boomerang)
     (W ProjectilePathSegment.cs:57-63 / R client ProjectilePathSegment.cs:111-119)
  + Amplitude: amplitude float32, frequency float32
  + Circle: radius float32
  + ChangeSpeed: Increment float32, Cooldown int32, CooldownOffset int32, Repeat int32
  Combined (replaces the base body): count byte, count x {type byte, segment body}, TimeOffset int32, Mods int32
  ```
  The overrides exist for exactly the same four types on both sides (server `AmplitudePath.cs:44`, `ChangeSpeedPath.cs:54`, `CirclePath.cs:40`,
  `CombinedPath.cs:50`; client `AmplitudePath.cs:46`, `ChangeSpeedPath.cs:57`, `CirclePath.cs:44`, `CombinedPath.cs:55`). The `PathType`
  enums are identical (`CMN/Resources/Xml/Descriptors/Enumerables.cs:3-13`, client `ProjectilePath.cs:127-138`). An unknown type reads as
  `null` on the client, and the rest of that packet is then mis-parsed (UNVERIFIED consequence).
- **Client**: spawns `NumShots` bullets with ids `WrapBulletId(First+i)` from the owner's projectile `ProjId` description.

#### EnemyHit (29, C->S)
- **Fields**: `BulletId: uint16`, `TargetId: int32`. W `Outgoing/EnemyHit.cs`; R `Systems/Combat/EnemyHit.cs:52-55`. **Match.**
- **Validation**: `Playing`; the target has a combat component (`:18-23`); on the game thread, the bullet exists for this shooter
  (`GetGlobalId`), `IsHitPlausible` (the bullet's path came within 2.0 tiles of the target's server position at some point in the last
  400 ms, `HitValidation.cs:11-15,20-34`), and per-bullet de-duplication (`Projectile.TryHitEntity`, `Projectile.cs:106-110`).
- **Client**: reports the first enemy its bullet sweeps (all of them for multi-hit) (`Game/Objects/Projectile.cs:264-292`).
  The damage number is **not** shown locally; it comes back as a server `Notification(IsDamage)`.

#### PlayerHit (28, C->S)
- **Fields**: `ObjectId (owner of the bullet): int32`, `BulletId: uint16`. W `Outgoing/PlayerHit.cs`; R `Systems/Combat/PlayerHit.cs:49-52`. **Match.**
- **Validation**: `Ready` + `Playing` (`:19`), owner and bullet exist, `IsHitPlausible` against **the sender's own player** (`:38`),
  de-duplication.
- **Note**: the server **also runs its own collision** every tick, for every projectile against its owner's target list at a 0.5-tile radius
  (`Projectile.cs:60-86`). A hit is therefore applied whether or not the client reports it. The client report is a lag-tolerant extra.

#### ServerProjectileProps (48, S->C)
- **Fields**: `ContainerType: uint16`, `ProjId: byte`, `ObjectId: UTF`, `Lifetime: float32`, `MultiHit: bool`, `PassesCover: bool`, `ArmorPiercing: bool`,
  `Size: int32`, `effectCount: uint16`, `effectCount x (Effect: uint16, Duration: int32)`.
  W `Systems/Projectiles/ServerProjectileProps.cs:19-34`; R `Incoming/ServerProjectileProps.cs` (Read). **Match.** The server
  `ConditionEffectIndex` (`CMN/Enumerables.cs:254`) and the client `ConditionEffect` (`WaW-Client/WaWClient/Game/ConditionEffect.cs:171`)
  have the same numbering for 0-43 (spot-checked); the tail is UNVERIFIED.
- **Server**: one packet per custom projectile, sent at accept (`RealmManager.cs:93-104`, which has a "TODO ... 1 packet" comment).
- **Client**: overrides the projectile description, and writes a `Console.WriteLine` per packet (debug leftover).

### 5.4 Inventory and items

`SlotObject` = `ObjectId: int32`, `SlotId: byte` (server `CMN/Structs/SlotObjectData.cs:10-16`, client `CLI/Structs/DataObjects/ObjectSlot.cs`). **Match.**

#### InvSwap (12, C->S)
- **Fields**: `SlotObject1`, `SlotObject2`. W `Outgoing/InvSwap.cs`; R `Systems/Inventory/InvSwap.cs:30-33`. **Match.**
- **Validation** (game thread, `EntityInventoryManager.cs:39-62` and the following methods): admin-only gear, same slot, backpack locked,
  potion stacks, power-up slot, equippable per slot; containers: `OwnedBy(account)` and **reach <= 3 tiles** (`:182-185,250-259,287-295`).
  The packet handler has **no `GameState` check**: `user.GameInfo.World` may be null, which makes it throw and disconnect the user with
  "Internal error" (`InvSwap.cs:27`).
- **Server reply**: `InvResult(0 = ok, 1 = refused)` (`EntityInventoryManager.cs:57`). The changed slots arrive as stats in the next NewTick.

#### InvResult (18, S->C)
- **Fields**: `Result: int32`. W `Systems/Inventory/InvResult.cs:9-11`; R `Incoming/InvResult.cs`. **Match.** Client `Handle` is a `//todo`
  no-op.

#### InvDrop (17, C->S)
- **Fields**: `SlotObject`. R `Systems/Inventory/InvDrop.cs:47-50`. **Match.**
- **Validation**: role gate, `ObjectId == own player`, backpack lock, slot not empty (`:23-37`). No `Playing` check at the packet layer.
- **Server**: removes the item and drops a public loot bag at the player's feet.

#### UseItem (13, C->S)
- **Fields**: `Time: int32`, `SlotObject`, `ItemUsePos: 2 x float32`, `UseType: byte`. W `Outgoing/UseItem.cs`; R `Systems/Inventory/UseItem.cs:217-222`. **Match.**
  `Time`, `Pos` and `UseType` are read but not used by the shown logic (UNVERIFIED for all item kinds).
- **Validation**: `Playing`, not dead, role gate (`:30-33`). From a bag: not in the Campsite, the bag is a container, owned/public, reach <= 3 tiles
  (`:75-91`). Skin-unlock items go through RPC `UnlockSkin`.
- **Server reply**: `Notification`s ("+N HP" etc.) and stat changes.

#### UsePowerUp (75, C->S)
- **Fields**: `Type: uint16`. R `PowerUps.cs:220-222`. **Match.** Logic in `PowerUps.Use` (game thread).

#### CatAction (76, C->S)
- **Fields**: `ObjectId: int32`, `Action: byte (0 Pet, 1 Feed)`, `Slot: byte`. W `Outgoing/CatAction.cs`; R `CatCare.cs:159-163`. **Match.**
- **Validation**: action in {Pet, Feed} (`:169`); reach, timing and food are checked in `CatCare.Apply` (not reviewed in detail).

#### ChangeSkin (74, C->S)
- **Fields**: `Skin: uint16`. R `Session/SkinChange.cs:21-23`. **Match.**
- **Validation** (`SkinChange.Apply`, `:39-66`): a character exists and is not dead, not in the Tutorial, the skin differs, and
  `SkinRules.CanWear` (owned or free, right class). The result is the public stat `Texture` plus a `Notification`.

#### BuyStorage (71, C->S) / StorageInfo (72, S->C)
- **BuyStorage fields**: none. Flow `StorageShop.Buy` (`StorageShop.cs:40-85`): one purchase in flight per account; a game-thread check (own
  Campsite, below max, reach <= 3 tiles to the Storage, `:88-107`); RPC `BuyStorage(accountId, expectedRows)`; apply. **The handler awaits**,
  so this user's later packets wait (up to 10 s per step).
- **StorageInfo fields**: `Rows: int32`, `MaxRows: int32`, `Price: int32`, `Ok: bool`, `Message: UTF`. W `StorageShop.cs:138-144`; R `Incoming/StorageInfo.cs`. **Match.**

#### ClaimStarter (69, C->S) / StarterOffer (68, S->C) / StarterResult (70, S->C)
- **ClaimStarter**: no body. Grant runs on the game thread (Nexus, `StarterPending`, inventory ready), then RPC `ClaimStarter` (`ClaimStarter.cs:21-40,69-98`).
- **StarterOffer fields**: `count: int16`, `count x itemType: uint16`. W `ClaimStarter.cs:105-110`; R `Incoming/StarterOffer.cs`. **Match.**
- **StarterResult fields**: `Ok: bool`, `Message: UTF`. W `ClaimStarter.cs:117-120`; R `Incoming/StarterResult.cs`. **Match.**

#### ClaimBounty (78, C->S)
- No body. `Playing` check, then `BountyBoard.Claim` (fire-and-forget async with RPC `ClaimBounty`) (`BountyBoard.cs:119-129`). The result
  arrives as the private string stat `BountyState` plus chat `Text`.

### 5.5 Chat and portals

#### PlayerText (5, C->S)
- **Fields**: `Text: UTF`. R `Systems/Chat/PlayerText.cs:21-23`. **Match.**
- **Validation**: `Playing`, role gate (`:16`), then `PlayerChat.ValidateSpeak`: max 256 characters, 500 ms cooldown, mute
  (`PlayerChat.cs:13-14,29-48`). Slash commands are handled server-side (UNVERIFIED path, `Systems/Chat/Commands`).

#### Text (6, S->C)
- **Fields**: `Name: UTF`, `ObjectId: int32`, `NumStars: int32`, `BubbleTime: byte`, `Recipient: UTF`, `Txt: UTF`. W `Systems/Chat/Text.cs:11-18`; R
  `Incoming/Text.cs`. **Match.**
- **In-band conventions**: `Name = "*Error*"` / `"*Help*"` / `""` (info), `Recipient = "*Party*"` (`Systems/Chat/ChatExtensions.cs:21-70`).
  `ObjectId = 0` and `NumStars = -1` mean system.

#### UsePortal (23, C->S)
- **Fields**: `ObjectId: int32`. R `Systems/Portals/UsePortal.cs:38-40`. **Match.**
- **Validation**: `Playing`, role gate, the id is a portal in this world, target world exists, not deleted, portal enabled (`:17-35`).
  **There is no distance check**: any portal in the world can be used from anywhere (by reading the code).
- **Server**: `ReconnectTo(target)`, or `Failure(5, msg, disconnect:false)`, which the client treats as fatal.

### 5.6 Shared structures

#### ObjectStatus (inside Update and NewTick)
```
ObjectId: int32
X: float32, Y: float32
statCount: byte
statCount x StatData { Type: byte, Value: (IsStringStat(Type) ? UTF : int32) }
```
- Server: header `ObjectStatusData.cs:27-30`. `WriteForUpdate` writes every stat that `HasValue` and whose index is set in `PrivacyMask`
  (`:32-47`). `WriteForNewTick` writes the delta list `StatUpdates[0..StatCount)` filtered the same way (`:49-64`). The count byte is
  back-patched (`:66-71`).
- Client: `ObjectStats.Read` (NewTick) and `ObjectDef.Read` (Update: `ObjectType` first) (`CLI/Structs/DataObjects/ObjectStats.cs`,
  `ObjectDef.cs`). The unused client `ObjectDef.Write` writes the count as **int16** (it reads a byte); it is never used.
- `StatData`: server `CMN/Structs/StatData.cs:55-86`, client `CLI/Structs/DataObjects/StatData.cs:17-38`. `IsFloatStat` is always false on both
  sides, so float stats do not exist on the wire.
- `StatCount <= 150` (`StatTypeCount`), so it fits in a byte.

#### TileData (Update)
`X: int16, Y: int16, GroundType: uint16`. Server `MapData.cs:141-145`, client `TileDef.cs`. Objects on tiles (walls, trees) are **not** in
the tile record. The client needs its own copy of the map (UNVERIFIED how the client learns static objects; CLAUDE.md implies they come
from the client's map files).

#### WorldPosData
`PROTO/WorldPosData.cs:6-13` (struct of two floats); wire = 8 bytes. The web build compiles a patched copy
(`WEB/web/patched/Shared__Common.Protocol__WorldPosData.cs`, `patched.props:4`). Assumed wire-neutral (UNVERIFIED in detail).

---

## 6. Tick and update model

### 6.1 Cadence
- `TPS = 20` (`gameServerConfig.xml:8`). `MsPT = 1000 / TPS = 50` (`CMN/Resources/Config/GameServerConfig.cs:21-22`). Loop at `GameLogic.cs:33-85`:
  `Update()` (drain actions, `world.Update`, **network I/O for all users**) runs every ~1 ms. Once 50 ms have passed, `TickWorlds`
  runs (parallel per world).
- World tick order (`SRV/Game/Worlds/World.cs:301-323`): timers, Zones, `Projectiles` (server collision), `Map`, `PortalDatas`,
  `EntityInventories` (inventory -> stats), `EntityCombat`, `EntityProjectiles`, `EntityBehaviors` (AI, enemy shots), Quests,
  **`PlayerSights` (builds and sends Update + NewTick)**, PowerUps, **`EntityStats.Tick` (collects this tick's changed stats into
  `StatUpdates`, clears `PositionUpdate`)**.

### 6.2 How stats are diffed
- `EntityStats.SetInternal` (`SRV/Game/Systems/Stats/EntityStats.cs:177-188`) ignores equal values. Otherwise it sets the value, marks
  `_statUpdatesMask`, and sets `PrivateMask` (always) and `PublicMask` (unless `isPrivate`). **Masks are sticky**: once a stat is public it stays
  public for the entity's lifetime.
- `EntityStats.Tick` (`:190-202`) turns the mask into the `StatUpdates[]` list (`StatUpdateCount`) and clears it.
- Because `PlayerSights.Tick` runs **before** `EntityStats.Tick` in the same world tick, a NewTick carries the stat changes collected at
  the **end of the previous tick**. Changes made by packet handlers between ticks are therefore sent 1-2 ticks (50-100 ms) later.
  Position changes (`PositionUpdate`, set by `Move`/AI) go out in the very next sight pass. (Derived from the tick order; UNVERIFIED by
  measurement.)
- Privacy: for the viewer's own player the `PrivateMask` is used, for everyone else the `PublicMask` (`PlayerSightManager.cs:227-228,251-252`).
  Note that `EntityStats.Set` defaults to public, so inventory slots and item data strings (`InventoryData*`) of every player are public
  (`EntityInventory.cs:228-230`). Which stats are set private was not exhaustively audited.

### 6.3 Visibility-driven Update and NewTick (`SRV/Game/Systems/Sight/PlayerSightManager.cs`)
- Sight radius 20 tiles (`:37-38`). Two modes per world config: `UNBLOCKED_SIGHT` (a disc) or `LINE_OF_SIGHT` (an 8-octant shadow-cast,
  `:91-173`).
- Tiles: a tile is sent once, the first time it becomes visible (`DiscoveredTiles`), or again when it is forced (`TileUpdate`) (`:175-181`).
- Entities: an entity is "new" the first time it is visible to this player. It is then sent in `Update` with a full public/private
  snapshot (`:232-254`). Entities that are no longer visible go into `Update.drops` (`:188-202`). Containers are visible only to players
  allowed to open them (`:286-291`).
- Each visible entity with `StatUpdateCount != 0 || PositionUpdate` contributes one `ObjectStatus` to this player's NewTick (`:212-230`).
- The quest monster is tracked beyond the radius (`:261-280`).
- Per-tick caches (`_entityDataCache`, `_entityStatusCache`) share the serialisable data between viewers within a world tick (`:46-51`).

### 6.4 Move / ack flow
```
server tick N:  NewTick ------------------------------>  client: queue Move(current local pos), then apply statuses
client       :  Move(pos) ------------------------------>  server: Playing? Goto pending? speed/wall check
                                                            ok  -> player.Move (PositionUpdate=true) -> region triggers
                                                            bad -> Goto(own id, last good pos) + AntiCheat points
client       :  on Goto: snap local player, GotoAck ---->  server: clear GotoSentMs, reset speed window
```
There is no input sequencing, no server reconciliation of client prediction, and no timestamps (`GotoAck.Time` and `UseItem.Time`
exist but are unused or zero).

### 6.5 Anti-cheat scoring
`CheatScore`: points drain 0.2 per second; at 20 the session is logged as suspicious; at 60 the server kicks with `Failure(0)` and flags the
account through RPC `FlagSuspect` (`SRV/Game/Systems/Combat/CheatScore.cs:3-15`, `AntiCheat.cs:13-45`).

---

## 7. Stat type enum

Server: `WaW-Server/Common/Enumerables.cs:82-222` (`enum StatType`, an int; on the wire as a byte). Client:
`CLI/Enums/StatsType.cs:3-147` (`enum StatsType : byte`). Ids 0-82 have **the same numbers** on both sides (names differ in places).
Above 82 **they differ**, as CLAUDE.md:522 also states.

| Id | Server name | Client name | Wire type | Notes |
|---|---|---|---|---|
| 0 | MaxHP | MaximumHp | int | |
| 1 | HP | Hp | int | |
| 2 | Size | Size | int | |
| 3 | MaxMP | MaximumMp | int | |
| 4 | MP | Mp | int | |
| 5 | NextLevelXp | NextLevelXp | int | |
| 6 | Experience | Experience | int | |
| 7 | Level | Level | int | |
| 8-19 | Inventory0-11 | Inventory0-11 | int | item type, -1 = empty |
| 20 | Attack | Attack | int | |
| 21 | Defense | Defense | int | |
| 22 | Speed | Speed | int | |
| 23 | Vitality | Vitality | int | |
| 24 | Wisdom | Wisdom | int | |
| 25 | Dexterity | Dexterity | int | |
| 26 | Condition1 | Condition1 | int | never `Set` by the server (no call found), so condition effects are not synced through stats |
| 27 | NumStars | NumStars | int | |
| 28 | Name | Name | **string** | both lists |
| 29 | Tex1 | Texture1 | int | |
| 30 | Tex2 | Texture2 | int | |
| 31 | MerchandiseType | MerchandiseType | int | |
| 32 | MerchandisePrice | MerchandisePrice | int | |
| 33 | Credits | Credits | int | |
| 34 | Active | Active | int | |
| 35 | AccountId | AccountId | int | |
| 36 | Fame | Fame | int | |
| 37 | MerchandiseCurrency | MerchandiseCurrency | int | |
| 38 | Connect | Connect | int | |
| 39 | MerchandiseCount | MerchandiseCount | int | |
| 40 | MerchandiseMinsLeft | MerchandiseMinsLeft | int | |
| 41 | MerchandiseDiscount | MerchandiseDiscount | int | |
| 42 | MerchandiseRankReq | MerchandiseRankReq | int | |
| 43 | CharFame | CharFame | int | |
| 44 | NextClassQuestFame | NextClassQuestFame | int | |
| 45 | LegendaryRank | LegendaryRank | int | |
| 46 | SinkLevel | SinkLevel | int | |
| 47 | AltTexture | AltTexture | int | |
| 48 | **GuildName** | **Guild** | **string** | both lists (different names) |
| 49 | GuildRank | GuildRank | int | |
| 50 | Oxygen | Oxygen | int | |
| 51 | HealthPotionStack | HealthPotionStack | int | |
| 52 | MagicPotionStack | MagicPotionStack | int | |
| 53-60 | Backpack0-7 | BackPack0-7 | int | |
| 61 | HasBackpack | HasBackpack | int | client handler is `//todo` |
| 62 | Texture | Texture | int | skin |
| 63-82 | InventoryData0-19 | InventoryData0-19 | **string** | item export string; both lists |
| 83 | **Glow** | **PrimaryConstellation** | int | **mismatch** (server never sets Glow) |
| 84 | **AltTextureIndex** | **SecondaryConstellation** | int | **mismatch, live**: the server sets 84 (`Behaviors/Actions/SetAltTexture.cs:31,50`); the client files it under SecondaryConstellation, so alt textures never reach `Entity.SetAltTexture` |
| 85 | **PortalUsable** | **PrimaryNodeData** | int | mismatch (server never sets it) |
| 86-98 | Unused, Unused3, Unused2, Unused6-12, Unused4(96), Unused13, Unused14 | SecondaryNodeData(86), DodgeChance, CriticalChance, CriticalDamage, MaxMS, MS, ManaRegeneration, MSRegenRate, Armor, DamageMultiplier(95), Unused96, StatPoints(97), TimeInCombat(98) | int | server never sends |
| 99 | MaxHPBonus | MaxHpBonus | int | |
| 100 | MaxMPBonus | MaxMpBonus | int | |
| 101 | AttackBonus | AttackBonus | int | |
| 102 | DefenseBonus | DefenseBonus | int | |
| 103 | SpeedBonus | SpeedBonus | int | |
| 104 | DexterityBonus | DexterityBonus | int | |
| 105 | VitalityBonus | VitalityBonus | int | |
| 106 | WisdomBonus | WisdomBonus | int | |
| 107-117 | Unused15-22, Unused5(115), Unused23, Unused24 | DodgeChanceBonus, CriticalChanceBonus, CriticalDamageBonus, MaxMSBonus, ManaRegenerationBonus, MSRegenRateBonus, ArmorBonus, DamageBonus, Unused115, AttackSpeed(116), AttackSpeedBonus(117) | int | server never sends |
| 118 | Condition2 | Condition2 | int | |
| 119 | AccRank | AccRank | int | |
| 120 | QuestId | QuestId | int | |
| 121 | Unused25 | PartyId | int | |
| 122-125 | Unused26-29 (**int**) | AbilityDataA-D (**string** on the client: `StatData.cs:70-73`) | **type mismatch** | would desync the stat list (but not the frame) if the server ever sent them |
| 126 | (none) | Effects | - | client only |
| 127 | (none) | Glow | - | client only (the client's real Glow) |
| 128 | (none) | AltTextureIndex | - | client only |
| 129 | (none) | PortalUsable | - | client only |
| 130 | (none) | Skin | - | client only |
| 131 | Role | Role | **string** | both lists |
| 132-145 | (free) | (free) | - | ex-Skill slots |
| 146 | PowerUps | PowerUps | **string** | `"type:count,..."` private |
| 147 | PowerUpsActive | PowerUpsActive | **string** | `"type:msLeft,..."` |
| 148 | Unused148 | Unused148 | - | ex-Jumps |
| 149 | BountyState | BountyState | **string** | `"day:progress:claimed"` private (`PROTO/Bounties.cs:23`) |
| 150 | StatTypeCount | StatTypeCount | - | sentinel |
| - | None = int.MaxValue | None = 255 | - | |

The string-stat lists (`IsStringStat`) are server `CMN/Structs/StatData.cs:21-53` and client `CLI/Structs/DataObjects/StatData.cs:40-78`.
They agree except that the **client adds 122-125**. A dead `StatsTypeOld` enum with old numbering (Vitality=26 ...) is still in the client
(`StatsType.cs:149-226`).

---

## 8. GameServer <-> AccountServer RPC (second protocol)

### 8.1 Transport
- TCP to `RpcClientConfig.ServerHost:ServerPort` = 127.0.0.1:8081 (`CMN/Resources/Config/Data/rpcClientConfig.example.xml`). The server listens on
  `ListenAddress:ListenPort` = 127.0.0.1:8081 (`rpcServerConfig.example.xml`).
- **TLS** (`SslStream`). The server certificate comes from `CertificatePfxPath`. The client validates with a **pinned certificate**
  (`RpcCertificateHelper.ValidatePinned`) and does not use client certificates (`CMN/Messaging/IpcClient.cs:12-30`,
  `IpcServer.cs:20-84`).
- **Shared-secret handshake** inside TLS, before any JSON-RPC (`CMN/Messaging/RpcHandshake.cs`): the client sends
  `int32 (BitConverter, host order = LE on x86/x64) length + UTF-8 secret`. The secret may be at most 4096 bytes (`:13`). The server answers one
  byte, 1 = ok or 0 = rejected, and closes on a reject (`:15-45`). The comparison is plain `==` (not constant-time).
- **JSON-RPC** via StreamJsonRpc 2.25.29 (`CMN/Common.csproj:31`). Both ends use `new JsonRpc(stream)` / `JsonRpc.Attach(stream, ...)` with no
  custom formatter. In this library's defaults that means HTTP-like `Content-Length` header framing with a JSON formatter (UNVERIFIED in this
  library version). The interfaces carry `[JsonRpcContract]` and `[GenerateShape]` (PolyType) attributes (`CMN/Messaging/Proxies.cs:10-11,41-42`).
- The connection is **bidirectional**: the GameServer is the TCP client, and each side exposes an interface to the other.
- Lifecycle: the GameServer connects at start (5 s timeout) and calls `GameServerConnected(guid)` (`SRV/Program.cs:47-52`). When the
  connection drops it reconnects every 5 s and re-registers (`Program.cs:84-110`). The AccountServer releases that server's account locks
  when the connection ends (`IpcServer.cs:71-84`, `AccServerRpcHandler.cs:24-27`). It also releases stale locks of servers that do not
  reconnect within 10 s of an AccountServer start (`WaW-Server/AccountServer/Program.cs` `ReleaseStaleLocksAsync`).

### 8.2 `IAccountServerRpc` (implemented by the AccountServer, called by the GameServer) - `Proxies.cs:43-95`, implementation `AccountServer/Messaging/AccServerRpcHandler.cs`

| Method | Args | Returns | Caller (GameServer) | Purpose |
|---|---|---|---|---|
| GameServerConnected | `Guid gameServerId` | `Task` | `Program.cs:49,100` | register this server (`IpcServer.Clients`); throws on a duplicate GUID |
| GetUserInfo | `string name, int accountId` | `GameInfoDto?` | `Chat/Commands/ModCommands.cs:39` | **forwards to the calling GameServer's own `GetUserInfo`** (`AccServerRpcHandler.cs:38-40`), so it only finds players on the same server |
| VerifyAccount | `string username, string password, Guid gameServerGuid` | `VerifyResultDto(Account Acc, VerifyStatus Status)` | `Hello.cs:53` | credentials + account lock |
| GetActiveBans | `int accountId` | `BanRecord[]` | `Hello.cs:76` | ban expiry check |
| FlushAccount | `Account account` | `Task` | `Hello.cs:87` | writes the **whole account object** sent by the GameServer (used to clear `IsBanned`) |
| GetCharacter | `int accountId, int charId` | `Character` | `Load.cs:32` | load a character |
| CreateCharacter | `Account account, ushort objectType, ushort skinType, string role` | `CreateCharacterResultDto(Account, Character, CreateCharacterStatus)` | `Create.cs:26` | validated creation |
| FindAccount | `string name` | `AccountBriefDto` | **no caller found** | moderation lookup |
| Moderate | `ModerationRequestDto(Action, ModeratorId, Target, Reason, DurationMinutes, Rank)` | `ModerationResultDto(Error)` | `ModCommands.cs:128,152,168,193`, `OwnerCommands.cs:63` | ban/unban/mute/unmute/setrank |
| GetMuteState | `int accountId` | `MuteStateDto(MuteEndUnix)` | `Hello.cs:137`, `ModCommands.cs:21` | mute end (0 = none, long.MaxValue = permanent) |
| SendMail | `MailRequestDto(Target, From, Subject, Body, Gold, Fame, Items?)` | `ModerationResultDto` | `OwnerCommands.cs:106` | inbox mail |
| SaveCampsiteChests | `int accountId, CampsiteChest[] chests` | `Task` | `Worlds/Logic/Campsite.cs:362` | targeted save |
| LoadGiftChest | `int accountId` | `int[]` | `Worlds/Logic/GiftChestStore.cs:18` | |
| SaveGiftChest | `int accountId, int[] chest` | `Task` | `GiftChestStore.cs:19` | |
| FillGiftChest | `int accountId, int[] chest` | `GiftFillDto(Chest, Placed, Waiting)` | `GiftChestStore.cs:20` | move queued gifts into the chest |
| SaveCharacter | `int accountId, Character chr` | `Task` | `Systems/Persistence/CharacterSaver.cs:114` | disconnect / switch / autosave (60 s) / shutdown |
| RecordDeath | `int accountId, Character chr` | `bool` | `Systems/Combat/PlayerDeath.cs:105` | mark dead, pay fame once |
| FlagSuspect | `int accountId, string accountName, int points, string summary` | `Task` | `Systems/Combat/AntiCheat.cs:38` | anti-cheat review row |
| ClaimStarter | `int accountId` | `bool` | `Session/ClaimStarter.cs:35` | clear `StarterPending` |
| BuyStorage | `int accountId, int expectedRows` | `StorageBuyDto(Ok, Error?, Rows, Gold)` | `Session/StorageShop.cs:65` | conditional purchase |
| UnlockSkin | `int accountId, ushort skinType` | `bool` | `Systems/Inventory/SkinUnlock.cs:70` | add owned skin |
| ClaimBounty | `int accountId, int gold, int fame` | `BountyClaimDto(Ok, Gold, Fame)` | `Systems/Rewards/BountyBoard.cs:88` | pay the bounty (**the amounts come from the GameServer**) |

### 8.3 `IGameServerRpc` (implemented by the GameServer, called by the AccountServer) - `Proxies.cs:12-26`, implementation `SRV/Messaging/GameServerRpcHandler.cs`

| Method | Args | Returns | Caller (AccountServer) | Purpose |
|---|---|---|---|---|
| GlobalAnnouncement | `string from, string message` | `bool` | **no caller found** | only logs (`GameServerRpcHandler.cs:18-21`) |
| GetGameServer | - | `ServerInfo(Guid, Type, UptimeMs, PlayerCount)` | `Systems/Public/PublicHandlers.cs:42` | online count |
| GetUserInfo | `string name, int accountId` | `GameInfoDto?(AccountId, WorldId, WorldName, Position)` | `PublicHandlers.cs:30`, and via the forwarder above | where a player is. Dereferences `c.GameInfo.Account.Id` for all users, including not-yet-logged-in ones whose Account is null (NRE risk) (`GameServerRpcHandler.cs:27-30`) |
| GetStatus | - | `GameServerStatusDto(...)` | `Systems/Dev/DevHandlers.cs:94` | dashboard |
| ApplyModeration | `int accountId, string action ("kick","ban","mute","unmute"), string message` | `bool` | `DevHandlers.cs:59` | runs on the game thread (`GameLogic.Enqueue`) |
| GetWeather | - | `WeatherDto` | `DevHandlers.cs:308` | |
| SetWeather | `string weather, string time, string by` | `WeatherDto` | `DevHandlers.cs:308` | broadcasts `WeatherState` |

DTOs are serialised as JSON. Some carry full persistence models (`Account`, `Character`, `BanRecord`, `CampsiteChest`), so **the DB model is part of
the RPC contract** (UNVERIFIED which fields are serialised).

### 8.4 AccountServer HTTP (route list only; the AccountServer audit covers the depth)

- Listener: `HttpListener` on `AppEngineConfig` `127.0.0.1:8080` (`appEngineConfig.xml`). The desktop client uses
  `http://<host>:8080` (`WaW-Client/WaWClient/Core/Settings.cs:36-41`). The browser uses same-origin `/api/` through nginx (`setup_web.sh` `location /api/`).
- Request: **POST with a form-urlencoded body** (the client uses `FormUrlEncodedContent`, `WaW-Client/WaWClient/AppEngine/AppEngineClient.cs:32,40`).
  URL query parameters are merged in for GETs. The body is limited to `MaxRequestBodyBytes`, and larger bodies get 413
  (`AccountServer/Program.cs:176-209`). Routing is an exact match on `Url.LocalPath` to `RequestHandler.Path`; an unknown path closes the
  response with no body (`Program.cs:164-172`; `Systems/RequestHandler.cs:33-54`).
- Response: a text body. It is **XML** (`<Error>msg</Error>`, `<Success/>`, or a handler-specific element such as `<Version downloadUrl="...">0.3.16</Version>`)
  with `Content-Type: text/*`, or JSON for `/public/*` (`application/json`, CORS `*`) (`Program.cs:215-236`, `RequestHandler.cs:20-30`).
  Authentication is `username` + `password` in each request (per F30 / the audit; the parameters are confirmed by grep below).

| Route | Parameters seen (`query["..."]`) |
|---|---|
| `/account/verify`, `/account/remember` | username, password |
| `/account/register` | newUsername, newPassword |
| `/account/forget` | username, token |
| `/account/purchaseCharSlot` | username, password |
| `/account/purchaseSkin` | username, password, skinType |
| `/app/version` | - (returns `<Version downloadUrl>`) |
| `/char/list`, `/char/delete`, `/char/chooseRole` | username, password, charId, role |
| `/char/fame` | accountId, charId |
| `/fame/list` | timespan |
| `/news/feed` | - |
| `/board/list`, `/board/post`, `/board/delete`, `/board/status` | username, password, id, message, status |
| `/inbox/list`, `/inbox/read`, `/inbox/claim`, `/inbox/delete`, `/daily/status`, `/daily/claim`, `/daily/spin` | id (+ credentials through a helper, UNVERIFIED) |
| `/music/now`, `/music/skip`, `/music/set` | dir, track |
| `/public/player`, `/public/search`, `/public/leaderboard`, `/public/guild`, `/public/releases`, `/public/online` | name, q, kind (JSON responses) |
| `/dev/whoami`, `/dev/status`, `/dev/players`, `/dev/player`, `/dev/moderate`, `/dev/mail`, `/dev/anticheat`, `/dev/news/list`, `/dev/news/post`, `/dev/news/delete`, `/dev/weather` | username, password, action, target, reason, minutes, rank, subject, body, gold, fame, title, text, author, version, weather, time, q, name, id, limit |
| `/crossdomain.xml` | - |
| (`/guild/getBoard`, `/guild/setBoard`, `/guild/listMembers`) | commented out, not registered |

---

## 9. Assessment for the new architecture

### 9.1 What is sound and worth keeping
1. **Length-prefixed binary framing with a body slice per frame** (section 1.2). A bad body cannot desync the stream. This is easy to
   implement identically in C# and C++.
2. **Little-endian, fixed-width primitives, UTF-8 strings with a u16 length.** Simple, portable, no varint ambiguity.
3. **One shared id table** with "append only, never renumber" discipline and a test that pins it (`PacketId.cs`, `PacketIdTests.cs`).
4. **Interest management on the server** (sight radius, line of sight, discovered-tile memory, per-viewer privacy masks, enter/leave via
   `Update`) and **per-tick delta stats** in `NewTick`. Conceptually this is the right model for an authoritative server.
5. **Server-side plausibility checks**: movement speed and walls with snap-back, a fire-rate bucket, hit plausibility over a lag window,
   per-bullet de-duplication, server-side projectile collision, and anti-cheat points. Damage numbers are server-originated.
6. **Server-chosen projectile spawn position** (`PlayerShoot.cs:70`). The client supplies only the angle.
7. **Version gate** at Hello and over HTTP, and a `Failure` flushed before close.
8. **RPC hardening**: TLS with a pinned certificate plus a shared secret, game-thread marshalling, and lock release on disconnect.

### 9.2 What is unsuitable

| # | Problem | Evidence |
|---|---|---|
| U1 | **Client-authoritative movement.** The client simulates and reports absolute positions. The server only checks plausibility, and the slack is ~2.25x max speed + 1 tile | `Move.cs:39-54`, `PlausibilityRules.cs:12-21` |
| U2 | **Client-reported hits** (`EnemyHit`/`PlayerHit`) with a 2-tile, 400 ms acceptance window. Redundant with server collision, but it widens what a cheat can claim | `HitValidation.cs:11-15`, `Projectile.cs:60-86` |
| U3 | **Implicit bullet ids** (the client and server counters must stay in lock-step, wrapping at 2000). Fragile, and impossible over an unreliable transport | `PlayerShoot.cs:29-34,58-66` |
| U4 | **No ticks, sequence numbers or timestamps** in Move, NewTick or shots, so no lag compensation and no reconciliation. `GotoAck.Time`/`UseItem.Time` exist but are unused | 5.2, 6.4 |
| U5 | **String-heavy stats**: item data (`InventoryData0-19`), power-ups, bounty and role are free-form strings in the stat stream; whether a stat is a string is decided by a hand-kept switch in three places. A miss broke every Update packet (CLAUDE.md:969-970) | `StatData.cs` (both) |
| U6 | **Divergent stat enums** above id 82; one live bug (AltTextureIndex 84) | section 7 |
| U7 | **Hand-written serializers duplicated** with no shared schema. Live mismatches: AccountList, Notification and ShowEffect colours, the ShowEffect layout | section 10 |
| U8 | **Plain-text credentials in every Hello** (and re-sent on each world switch). No session token, no TLS for desktop | 3.3 |
| U9 | **Same-socket Reconnect trusts the client's next Hello GameId** (it is not compared with the id the server issued), with no re-auth | `Hello.cs:52,98`, `User.cs:122-131` |
| U10 | **No login/idle timeout**, and **no keep-alive**. Browser players are exempt from the per-IP cap because they all come through loopback | 3.10, 1.6 |
| U11 | **Inconsistent count prefixes** (int16/uint16/byte/int32) and inconsistent angle units (radians C->S, degrees S->C) | 2, 5.3 |
| U12 | The full DB models (`Account`, `Character`) cross the RPC boundary, and the GameServer can write a whole Account (`FlushAccount`) | 8.2 |
| U13 | Server send-buffer resize recursion bug; the web client cannot send packets > 64 KiB; the client receive limit (256 KiB) is larger than the server's (128 KiB) | 1.3 |
| U14 | `UsePortal` has no distance check | 5.5 |

### 9.3 RECOMMENDATION (new Unity C# client + C++ authoritative server)

> This subsection is a recommendation, not a description of existing code.

1. **A single schema as the source of truth.** Define every message, enum (packet ids, stat ids, condition effects, path types, failure
   codes, effect types) and shared struct in one IDL. Options: FlatBuffers, or a small custom `.yaml`/`.json` schema with a generator.
   Generate the C# codec (Unity, no reflection, allocation-free readers into pooled structs) and the C++ codec from it. CI must fail when
   the generated code is stale. Golden-byte tests: one serialized sample per message, decoded by both generated codecs.
2. **Framing**: keep `[u32 LE length][u16 message id][payload]`, but count `length` as payload-only (or keep "inclusive" and document it),
   widen the id to u16, and add a **u8 flags** byte for compression and future channels. Cap the inbound size on both sides to the same
   configured value (e.g. 256 KiB), and bound the send queue per connection. Optionally add LZ4 for `Update` above a threshold.
3. **Versioning**: a `Hello{protocolVersion:u32, buildVersion:string, schemaHash:u64}`. Reject on a protocol mismatch with a structured
   `Failure{code, fatal:bool, message, requiredVersion}`. Keep the HTTP `/app/version` pre-check.
4. **Authentication**: log in over HTTPS to the account service, which returns a short-lived **session token**. `Hello` carries the token,
   never the password. World switches use a **server-issued, single-use transfer ticket** bound to the target world id (fixes U8 and U9).
   Use TLS on the game connection for desktop too (or a token-only design where the game socket carries no secrets, plus TLS via the WSS
   path).
5. **Authority**:
   - Movement: the client sends **inputs** (`MoveInput{seq:u32, clientTick, dir:vec2 or target pos, dt}`). The server simulates and sends
     `PlayerState{lastProcessedSeq, pos, vel}`, and the client reconciles. If full input authority is too costly for the first version,
     keep position reporting but add `seq` and server time, and tighten the speed slack.
   - Combat: the server is authoritative for all hits. Drop `EnemyHit`/`PlayerHit` (the server already collides). If a feel-good
     client-side hit is wanted, add lag compensation on the server (rewind targets by the measured RTT), not trust.
   - Shots: `Shoot{seq, clientTick, weaponSlot, angle}` with an **explicit bullet id** echoed by the server, never an implicit counter.
6. **Snapshots**: keep the Update/NewTick split (enter/leave + deltas) but make it explicit: `WorldSnapshot{serverTick, ackedInputSeq,
   entered[], left[], deltas[]}` with per-component delta bitmasks instead of the `(statId, value)` list. Quantize positions (e.g.
   int32 fixed-point 1/256 tile, or u16 within a chunk) and use typed fields instead of string stats. Item instances should be a typed
   struct `{itemType:u16, flags, rolls[]}` and not an export string. Send stat changes in the same tick they happen (fix the 1-2 tick
   ordering latency).
7. **Keep-alive**: `Ping{t}`/`Pong{t}` every 1-2 s, used for RTT (needed by lag compensation). Server timeouts: Hello within 5 s,
   idle 15-30 s. Apply per-connection caps after the bridge (honour `X-Forwarded-For` from the bridge, or have the C++ server speak
   WebSocket directly).
8. **Transport**: TCP plus a WebSocket gateway is acceptable for this genre if snapshots stay small. If UDP is ever wanted, the steps
   above (explicit ids and seqs, idempotent snapshots) are prerequisites. Do not port the implicit counters.
9. **Keep 1:1 (semantics)**: the packet roles (Hello/MapInfo/Load/Create/CreateSuccess, Update/NewTick, Goto, Text, Notification,
   EnemyShoot + ProjectilePath model, ServerProjectileProps, inventory slot addressing `(objectId, slot)`, StorageInfo, Starter*,
   WeatherState, QuestObjId, ClaimBounty, CatAction, UsePowerUp, ChangeSkin), the world-id convention (`WorldIds`), the sight radius,
   the projectile path math (it must be bit-identical between C# and C++: write shared test vectors), and the anti-cheat scoring
   thresholds as a starting point.
10. **Change or drop**: all dead ids (trade, guild, Aoe, Damage, PlaySound, Buy, Teleport, AoeAck, Reskin, Ping/Pong as they stand,
    UpdateAck, EditAccountList), the string stats, the divergent stat enum (renumber densely in the new schema; no wire compatibility
    with the old client is needed, which is UNVERIFIED as a project decision), the `Failure`-always-fatal client behaviour, colour as
    raw int (define `rgba8` explicitly), and the hand-written `IsStringStat` switches.
11. **Inter-server protocol**: replace JSON-RPC carrying DB models with explicit DTOs in the same schema (or gRPC/protobuf if the C++ server
    talks to the account service). Make writes intent-based (`ClearBan(accountId)`, not `FlushAccount(Account)`). Keep TLS and pinning;
    use a constant-time secret comparison or mTLS.

---

## 10. Discrepancies

### 10.1 Code vs code (client vs server)

| # | What | Server | Client | Effect |
|---|---|---|---|---|
| D1 | Stat enum ids 83-130 | `Glow=83, AltTextureIndex=84, PortalUsable=85`, 86-98 and 107-117, 121-125 unused (`CMN/Enumerables.cs:166-208`) | `PrimaryConstellation=83 ...`, `Glow=127, AltTextureIndex=128, PortalUsable=129, Skin=130` (`CLI/Enums/StatsType.cs:87-136`) | **live bug**: server stat 84 (AltTextureIndex, set at `Behaviors/Actions/SetAltTexture.cs:31,50`) is read by the client as SecondaryConstellation. (Whether any behaviour in use instantiates `SetAltTexture` is UNVERIFIED; no reference was found in `Behaviors/Library`.) |
| D2 | AccountList body | `int32 id, uint16 count, int32[]` (`Session/AccountList.cs:11-14`) | `int32 id, int16 count, UTF[]` parsed with `int.Parse` (`Incoming/AccountList.cs:19-22`) | a non-empty list throws in Read and the packet is dropped (party/lock/ignore data never arrives) |
| D3 | Colour in Notification / ShowEffect | `Write(int Color)`, an RGB int, so bytes `BB GG RR 00` (`Notification.cs:13`, `ShowEffect.cs:19`) | reads `A,R,G,B` bytes (`BGRA.cs` `ARGB.Read`; `Notification.cs:38,50`) | notification text colours are channel-rotated (by reading the code; not checked visually) |
| D4 | ShowEffect layout | fixed `type, target, color, param, pos1, pos2` (`ShowEffect.cs:16-23`) | per-type variable layout (`Incoming/ShowEffect.cs:23-110`) | wrong fields; harmless only because `Handle` is empty |
| D5 | Non-fatal Failure | `SendFailure(PORTAL_DISABLED, ..., disconnect:false)` (`UsePortal.cs:28-33`, `RegionTriggers.cs:38,64`) | `Failure.Handle` always `Client.Disconnect` (`Incoming/Failure.cs:36`) | a refused portal or the guild door kicks the client to the character list while the server keeps the session (until the socket closes) |
| D6 | String stats 122-125 | int (unused) | string (`CLI/Structs/DataObjects/StatData.cs:70-73`) | latent desync of the rest of that object's stats and later objects in the same packet |
| D7 | `Create` class/skin types | read as `int16` (`Session/Create.cs:20-21`) | written as `uint16` | wire-identical, semantic sign difference only |
| D8 | Client ids vs enum | `EditAccountList`(33) and `TradeRequest`(55) exist in the enum | the client classes return `PacketId.Unknown` (`Outgoing/EditAccountList.cs:8`, `Outgoing/RequestTrade.cs:6`) | never sent; `PartyData` calls `EditAccountList` in vain |
| D9 | TradeItem | `int Item, int SlotType, bool Tradeable, bool Included` (`CMN/Structs/TradeItem.cs:11-16`) | adds `ItemData: UTF` (`CLI/Structs/DataObjects/TradeItem.cs`) | dead (trade not implemented) |
| D10 | Angle units | `PlayerShoot.Angle` read as radians (`Rad2Deg()` applied, `PlayerShoot.cs:73,80`) | `ServerPlayerShoot.AngleDeg` / `EnemyShoot.Angle` (degrees, UNVERIFIED for EnemyShoot) | inconsistent but matched |
| D11 | Receive limits | server max inbound frame 128 KiB | client max inbound 256 KiB; desktop client send up to 1 MiB, web 64 KiB | a client packet of 128 KiB to 1 MiB would get the client disconnected (theoretical) |
| D12 | `UpdateAck` | no handler | sent after every Update but with `PacketId.Unknown`, so dropped (`Incoming/Update.cs:66`) | dead traffic code |

### 10.2 Docs vs code

| # | Doc says | Code does |
|---|---|---|
| X1 | `Docs/EngineeringAudit.md:58-59`: client packet ids are "a hand written enum (`Networking/Packets/PacketIds.cs`, plus a dead `PacketIdOld`)" | one shared enum `PROTO/PacketId.cs`; `CLI/Packets/PacketIds.cs:4` is a single `global using` alias; `PacketIdOld` is gone (the same audit's later entry at `:849-855` records the fix) |
| X2 | `EngineeringAudit.md:57`: outgoing "written into one 64 KB buffer" | grows from 64 KiB to 1 MiB, then drops packets (`CLI/SocketSendState.cs:15-16,81-91`) |
| X3 | `EngineeringAudit.md:93` (and F28 at `:254`): the server handles packets with `pkt.Handle(User).GetAwaiter().GetResult()`, blocking the game thread; F28 likewise | non-blocking: a pending handler task holds back only that user's queue (`NetworkHandler.cs:165-223`; recorded as fixed in `EngineeringAudit.md:675`) |
| X4 | `EngineeringAudit.md:90`: "There is **no sleep**: the loop busy-spins a core at 100%" | it sleeps 1 ms and spins only for the last <2 ms (`GameLogic.cs:48-58`) |
| X5 | `EngineeringAudit.md` F25: Move has "no speed, wall or teleport check", PlayerShoot "no cooldown check" | speed and wall checks are enforced with snap-back (`Move.cs:39-54`); fire-rate bucket (`PlayerShoot.cs:47-56`). A chat length limit now exists (256, `PlayerChat.cs:14`) |
| X6 | `EngineeringAudit.md` F26: no rate limit on incoming packets | warn at 200, disconnect above 2000 per drain (`NetworkHandler.cs:170-171,194-199`) |
| X7 | `EngineeringAudit.md` F27: `SocketServer._ips` is a plain Dictionary, "`MaxClientsPerIP` is 2000" | `ConnectionLedger`; `MaxClientsPerIP` = 10 (`gameServerConfig.xml:14`), loopback exempt |
| X8 | `EngineeringAudit.md` F24: "`Shared/Common.Protocol` holds only `LevelRules` and `WorldPosData`" | it also holds `PacketId`, `WorldIds`, `Bounties`, `Roles`, `InventoryLayout`, `ItemCategories`, `NexusCats`, `PowerUpList` |
| X9 | `EngineeringAudit.md:99` (1.4 Sessions): "Postgres + Redis lock" | `Proxies.cs:47-49` says "AccountServer is the sole process allowed to open alloy.db directly (Direct connection mode ...)". The configs include `postgresConfig.xml` and `redisConfig.xml`. Which store is live is out of scope here and flagged for the AccountServer audit (UNVERIFIED) |
| X10 | `CLAUDE.md:407`: "wire = int32 LITTLE-endian length incl. 5-byte header, byte id, body; UTF = uint16 LE + bytes" | **agrees** with the code |
| X11 | `CLAUDE.md:522`: "Client and server stat enums DIFFER after 82" | agrees; adds the live consequence D1 |
| X12 | `CLAUDE.md:773`: "The server's EffectType.Lightning packet is still read but not drawn" | the client reads ShowEffect with a layout that does not match the server's (D4); "read" is true only in the sense that the bytes are consumed |
| X13 | `CLAUDE.md:94`: "Systems/Quests.Tick once a second per world -> packet QuestObjId" | `Quests.Tick(this, time.TotalElapsedMs)` is called every world tick (`World.cs:317`); whether it throttles internally to 1 s was not checked (UNVERIFIED) |
| X14 | `WEB/web/shim/WebClient.cs:2` says "On the VPS a websockify bridge (wss -> 127.0.0.1:2050)" | the actual bridge is the custom `ws_bridge.py` (Python `websockets`), service `ww-bridge` (`setup_web.sh:8,19-34`) |
| X15 | `EngineeringAudit.md:60` (and F30): "username + password sent on every call" (HTTP) | confirmed by the route parameters (8.4); the game socket also re-sends them on every Reconnect |

---

## 11. Open questions

1. Is wire compatibility with the existing desktop/web client required during the migration (a mixed fleet), or can the new protocol be a
   clean break? This decides whether D1-D6 must be preserved or fixed.
2. Is `SetAltTexture` (stat 84) used by any live content (XML behaviours or `BehaviorLib`)? If yes, D1 is a visible bug today.
3. Should non-fatal `Failure` (code 5) stay a "Failure", or become a separate notice message? (D5: the current client always disconnects.)
4. Is the same-socket Reconnect GameId substitution (U9, `Hello.cs:98`) exploitable in practice, given `AllowsEntry` and `Dungeon` capacity
   checks? This needs a test against a running server, which is out of scope for this audit.
5. Which `EntityStats.Set(..., isPrivate: true)` calls exist? The privacy model is sticky-public. Are item data strings meant to be public?
6. Which fields of `Account`/`Character` are actually serialised over StreamJsonRpc (Newtonsoft vs System.Text.Json attributes)? This matters
   for the C++ side.
7. Does the client use `MapInfo.Seed` (and the server's `ClientRandom`) for anything shared, e.g. deterministic loot or projectile RNG? No
   use was found during this audit (UNVERIFIED).
8. Where does the client get static map objects (walls, trees)? `Update` tiles carry only the ground type. If the client has its own map
   files, the new protocol must either version them or send them.
9. Is the 1-2 tick stat latency (6.2) intentional?
10. Should browser connections keep going through a byte-pipe bridge, or should the C++ server accept WebSocket natively (which fixes the
    loopback-IP issue, U10)?
11. `ConditionEffect` (client) vs `ConditionEffectIndex` (server): full parity above id 43 is unchecked, and `Condition1/2` stats are never
    sent. How are condition effects meant to reach the client?

---

## 12. NEW PROTOCOL (implemented contract, version 1)

> This section describes the new protocol between the Unity client and the C++ GameServer. Sections 1-11 describe the reference.

### 12.1 Source of truth and generation
- Contract: `Protocol/schema/protocol.toml`. Generator: `python Protocol/generator/gen.py` (stdlib only), `--check` fails when any output is
  stale (the server's `build.cmd` runs it).
- Outputs: C++ `Server/libs/protocol/{include/waw/protocol/generated/messages.hpp, src/generated/messages.cpp}`, C#
  `WaW/Assets/Scripts/Protocol/Generated/Messages.cs`, golden vectors `Protocol/vectors/golden.json` plus test fixtures for both languages.
- Hand-written runtimes: C++ `bytes.hpp` / `framing.hpp`; C# `Runtime/ByteIO.cs` / `Runtime/Framing.cs`.
- Golden tests: the generator encodes one deterministic sample per message with an independent Python encoder. The C++ suite
  (`Server/tests/test_protocol.cpp`) and the C# suite (`WaW/Assets/Tests/EditMode/Protocol`, also runnable with
  `dotnet test Protocol/dotnet/WaW.Protocol.Tests.csproj`) must both produce and accept exactly those bytes.

### 12.2 Wire rules
| Rule | Value | Reference (for comparison) |
|---|---|---|
| Frame | `[u32 LE payload length][u16 LE id][payload]`, the length excludes the 6-byte header | `[i32 LE length incl. 5-byte header][u8 id]` |
| Max payload | 262144 bytes on both sides (`max_payload_bytes`) | 128 KiB server / 256 KiB client (D11) |
| Integers / floats | little-endian, fixed width, IEEE-754 | same |
| bool | u8, only 0 or 1 accepted | any non-zero byte |
| string | u16 LE byte count + UTF-8 (invalid UTF-8 is refused by the C# decoder) | same layout |
| list | u16 LE count + elements; a count larger than the remaining bytes is refused before allocating | mixed i16/u16/u8/i32 counts (U11) |
| enum | underlying unsigned int; unknown values refused | raw ints |
| optional fields | u32 presence mask first, then present fields in declaration order; unknown mask bits refused | n/a (stat id/value lists) |
| strictness | trailing bytes, wrong direction and unknown ids are errors, and the connection is closed | unknown ids ignored |
| ids | message ids and enum values are append-only; client->server ids 1-99, server->client 101+ | single shared enum |

### 12.3 Messages in version 1
Client -> server: Hello(1){protocolVersion, buildVersion, token}, Ping(2), LoadCharacter(3), CreateCharacter(4), MoveInput(5){steps:
list<MoveStep{seq, dtMs, dirX, dirY}>}, Shoot(6){shotId, clientTimeMs, angle}, ChatSend(7), Escape(8), UsePortal(9).
Server -> client: HelloAck(101), Failure(102){code, fatal, message}, Pong(103), WorldInfo(104), TileData(105), PlayerSpawned(106),
Snapshot(107){serverTick, ackInputSeq, entered: list<EntityFull>, left: list<u32>, changed: list<EntityDelta>}, ChatMessage(108),
Notification(109). Messages for inventory, items, loot, portals, storage, quests, weather, bounties, cats and power-ups are added when each
subsystem is migrated (MigrationStatus.md), always following section 9.3.

### 12.4 Intentional differences from the reference (all deliberate)
1. Session token in Hello instead of username + password (U8); the token comes from the Account/API service.
2. Movement intents (MoveInput with sequence numbers) instead of absolute positions (U1); the Snapshot acknowledges the last applied input.
3. No EnemyHit / PlayerHit / OtherHit / SquareHit: the server detects every hit (U2). Shoot carries an explicit client shot id instead of an
   implicit counter (U3).
4. Typed entity state (EntityFull / EntityDelta with presence masks) instead of `(statId, value)` lists with string stats (U5, U6).
5. Failure carries `fatal`: a non-fatal refusal does not disconnect (D5).
6. Colours are explicitly `0xRRGGBBAA` (D3).
7. Every id from the reference's dead / retired list is gone (9.3 item 10).
