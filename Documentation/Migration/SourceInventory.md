# Source Inventory - Warriors & Wizards (reference source)

Audit phase only. Nothing in the reference source was modified, built, launched or deployed while writing this.

- **REF** = `C:\Users\cbart\Desktop\Runity\Reference - This Is The Source Being Ported To Unity\Warriors-and-Wizards-Testing`. Every path below is relative to REF.
- **Target architecture:** Unity C# client, C++ authoritative GameServer, a separate Account/API service, PostgreSQL and Redis.
- **Verification:** every fact was checked against the files on 2026-10-02. **UNVERIFIED** marks anything that could not be confirmed from the source alone.
- **Secrets:** none are reproduced here. Where a file contains one, this document says so and gives no value. "VPS host address" stands for the literal IP that appears in `deploy.ps1`, `VPS_SETUP.md`, `WaW-Client/WaWClient/Core/Settings.cs`, the setup scripts and a few tools.

---

## 0. Repository at a glance

| Top-level item | What it is | Size (source only, unless noted) |
| --- | --- | --- |
| `Archive/` | `ServerBehaviors/`: 17 archived upstream enemy-AI files (`*.cs.txt`) and a README. Not compiled. | ~0.8 MB |
| `DeveloperDashboard/` | Avalonia staff tool `WaW-DeveloperDashboard` and its test project | 10 .cs files, ~1.2k lines (bin/obj ~776 MB) |
| `Docs/` | `EngineeringAudit.md` (1047 lines), `ProjectHistory.md` (1854 lines), `3D_OBJECT_PHILOSOPHY.md` | ~0.3 MB |
| `ExtraAssets/` | Unused leftovers: 3 music files (`Main_Music.wav` is 8.4 MB), `OldHeroes/Players_32x32.png`, the original AI 3D-object philosophy text | ~12 MB |
| `Forums/` | `vps/setup_forums.sh` (NodeBB on the VPS). Contains no forum code. | 6 KB |
| `Installer/` | Avalonia `WaW-Installer`. It links most of its code from `Launcher/`. | 2 .cs files of its own (bin/obj ~571 MB) |
| `Launcher/` | Avalonia `WaWLauncher` 3.0.3 and its test project | 13 .cs files, ~1.9k lines (bin/obj ~1.4 GB) |
| `Portal/` | Static "RealmEye-style" website (`site/`) and `vps/setup_portal.sh` | ~0.7 MB |
| `Shared/` | `Common.Protocol` library and its tests | 10 .cs files, 539 lines |
| `Tools/` | `Admin/`, `E2E/`, `Editor/`, `Mac/`, `Portal/` | 17 files, ~0.3 MB |
| `WaW-Client/` | Desktop client solution: 8 projects plus tests | ~2,250 .cs files counted below |
| `WaW-Server/` | Server solution: AccountServer, GameServer, Common, 2 test projects | ~400 .cs files |
| `WebClient/` | Browser (WASM + WebGL2) build of the same client sources, its tools and VPS files | 19 .cs files and JS |
| `dist/` | **Build output** from `deploy.ps1` | **1,911 MB, 696 files** |
| `WebClient/dist/` | **Build output** from `build_web.py` (`site`, `site-aot`, `publish`, `publish-aot`, `aot-artifacts`) | **1,274 MB, 3,939 files** |
| Other build output | `WaW-Client/WaWClient/bin` 931 MB; `WaW-Client/WaWClient/Content/bin` 94 MB; `WaW-Server/bin` 36 MB; plus `bin/`/`obj/` under every project | - |
| Root files | `README.md`, `CLAUDE.md` (128 KB), `VPS_SETUP.md`, `deploy.ps1`/`.cmd`, `promote.ps1`/`.cmd`, `.gitignore`, `Campsite_preview.png` | - |

**Counts**

| Kind | Count | Notes |
| --- | --- | --- |
| Solutions | 2 `.sln` | `WaW-Client/WarriorsAndWizards.Client.sln`, `WaW-Server/WarriorsAndWizards.Server.sln`. There are no `.slnx` files. |
| C# projects | 23 `.csproj` | 17 product or tool projects and 6 test projects |
| `global.json` | 2 | One per solution |
| `Directory.Build.*` | 0 | None |
| Python scripts | 18 | |
| PowerShell scripts | 4 | `deploy.ps1`, `promote.ps1`, two `reset_characters_*.ps1` |
| cmd wrappers | 5 | |
| Shell scripts | 4 | |
| SQL files | 2 | `Schema.sql`, `reset_characters.sql` |

Not in REF:
- CI configuration, Dockerfiles, `.env`, nginx or systemd unit files on disk. The setup scripts generate the nginx and systemd files on the VPS.
- The public website `warriorsandwizards.com`. It is a separate "Website repo" on Cloudflare Pages, per `VPS_SETUP.md` section 10 and `CLAUDE.md`.

The test projects in each solution:
- **Client solution:** `WaWClient.Tests` and `Common.Protocol.Tests`.
- **Server solution:** `Common.Tests`, `GameServer.Tests` and `Common.Protocol.Tests` (the same project as in the client solution).
- **In neither solution:** `Launcher`, `Launcher.Tests`, `Installer`, `DeveloperDashboard`, `DeveloperDashboard.Tests`, `WebClient/web/web.csproj` and `enumgen`.

---

## 1. Component blocks

Each block has these fields: Name / Purpose / Language / Framework / Dependencies / Inputs / Outputs / Runtime / Used by / Depends on / Migration destination.

### 1.1 Desktop client (`WaW-Client/`)

#### WaWClient (`WaW-Client/WaWClient/WaWClient.csproj`)
- **Name:** WaWClient. The local build is `WaWClient.exe`. With `-p:DeployTarget=vps` or `-p:PublicExe=true` the assembly is named `WarriorsAndWizards`.
- **Purpose:** the game itself: screens, HUD, map, rendering, networking, UI, the in-client Portal and launch-login handling. 349 .cs files, ~39k lines.
- **Language:** C# (`LangVersion` preview, `EnablePreviewFeatures`, unsafe allowed).
- **Framework:** net10.0, Exe. `RollForward` Major, `TieredCompilation` false. Uses OpenTK 5.0.0-pre.16 (GLFW window and OpenGL 3.3 core).
- **Dependencies:**

  | Kind | Items |
  | --- | --- |
  | NuGet | BouncyCastle.Cryptography 2.7.0 (no usage found), Microsoft.Extensions.Logging 10.0.11, Microsoft.Extensions.Logging.Console 10.0.11, OpenTK 5.0.0-pre.16 |
  | Projects | `Shared/Common.Protocol`, `WaW.Audio`, `WaW.Common`, `WaW.UiLib` |
  | Build-only | `WaW.ContentBuilder` (`ReferenceOutputAssembly=false`) |
  | Analyzer | `WaW.ShaderSourceGen` |
  | Local tool | `dotnet tool restore` (MonoGame `mgcb` 3.8.1.303 tools in `.config/dotnet-tools.json`; likely legacy, UNVERIFIED) |

- **Inputs:**
  - `Content/` source: `Xmls/*.xml`, `Sheets`, `Fonts`, `Sound`, `Title`, `Ui`, `Game.atlas`, `Ui.atlas`, `Content.xml`.
  - Shaders in `AdditionalFiles/*.vert|frag`.
  - Environment variables:

    | Variable | Purpose |
    | --- | --- |
    | `WAW_LAUNCH_USER`, `WAW_LAUNCH_TOKEN` | Sign-in handed over by the launcher |
    | `WAW_LAUNCHER_ONLY` | Forces launcher-only sign-in in a local build |
    | `WAW_LOG` | Client log level |
    | `WAW_PERFTEST*` | Performance test modes |
    | `WAW_WEATHER`, `WAW_TIME` | Weather and time-of-day overrides |
    | `WAW_NO_STATIC_BAKE` | Switches off static-prop baking |

- **Outputs:**
  - `bin/<cfg>/net10.0[/<rid>]/` holding the exe and the `Content/` folder that the post-build ContentBuilder writes.
  - Settings and `settings.xml` in `%LocalAppData%\WaWClient`.
- **Runtime:**
  - Connects to the AccountServer over plain HTTP at `http://<address>:8080`. `Core/Settings.cs`: `AppEngineAddress` is `127.0.0.1`, or the VPS host address under `TARGET_VPS`.
  - Connects to the GameServer over raw TCP on 2050.
  - Needs an OpenGL 3.3 core GPU.
- **Used by:** players directly or through the Launcher; `WebClient/web` (compiles the same sources); `WaWClient.Tests`.
- **Depends on:** AccountServer and GameServer at runtime; the XML content shared with the server.
- **Migration destination:** **Unity client.** Rewrite in Unity C#. Gameplay rules and protocol code can be ported (see `Shared/Common.Protocol`), but rendering, UI and audio are replaced by Unity.

#### WaW.Common (`WaW-Client/WaW.Common/WaW.Common.csproj`)
- **Name:** WaW.Common
- **Purpose:** client utility library: colour, math, collections, structs and the StbImage wrapper. 9 files, ~1.5k lines.
- **Language:** C#
- **Framework:** net10.0 library
- **Dependencies:** Microsoft.Extensions.Logging.Abstractions 10.0.11, OpenTK 5.0.0-pre.16, ReFuel.StbImage 2.1.1
- **Inputs:** none
- **Outputs:** DLL
- **Runtime:** in-process
- **Used by:** every client project
- **Depends on:** none
- **Migration destination:** **Unity client.** Replace with Unity math and image types; port only the helpers that are still needed.

#### WaW.Engine (`WaW-Client/WaW.Engine/WaW.Engine.csproj`)
- **Name:** WaW.Engine
- **Purpose:** `GameWindow` loop, GL buffers and shaders, textures. 22 files, ~1.3k lines.
- **Language:** C#
- **Framework:** net10.0
- **Dependencies:** Microsoft.Extensions.Logging 10.0.11, OpenTK 5.0.0-pre.16, ReFuel.StbImage 2.1.1; project WaW.Common
- **Inputs:** none
- **Outputs:** DLL
- **Runtime:** OpenGL 3.3 core
- **Used by:** WaW.ContentReader, WaWClient (through UiLib), web
- **Depends on:** WaW.Common
- **Migration destination:** **RETIRE.** Unity replaces the engine.

#### WaW.UiLib (`WaW-Client/WaW.UiLib/WaW.UiLib.csproj`)
- **Name:** WaW.UiLib
- **Purpose:** sprite-tree UI framework (`SpriteRender`, fonts, input, signals). 32 files, ~4k lines.
- **Language:** C#
- **Framework:** net10.0
- **Dependencies:**
  - NuGet: Microsoft.Extensions.Logging 10.0.11, OpenTK 5.0.0-pre.16
  - Projects: WaW.Common, WaW.ContentReader; WaW.ShaderSourceGen as an analyzer
- **Inputs:** `AdditionalFiles/Ui.vert|frag`
- **Outputs:** DLL. Generated files go to `.GeneratedFiles`.
- **Runtime:** in-process
- **Used by:** WaWClient, web
- **Depends on:** ContentReader
- **Migration destination:** **RETIRE.** Replaced by Unity UI (uGUI or UI Toolkit). Keep the layouts only as visual reference.

#### WaW.Audio (`WaW-Client/WaW.Audio/WaW.Audio.csproj`)
- **Name:** WaW.Audio
- **Purpose:** OpenAL audio thread and ogg/mp3/wav streaming. 11 files, ~1k lines.
- **Language:** C#
- **Framework:** net10.0
- **Dependencies:** Microsoft.Extensions.Logging 10.0.11, NLayer 1.16.0, OpenTK.Audio / Core / Mathematics 5.0.0-pre.16, Silk.NET.OpenAL.Soft.Native 1.23.1, StbVorbisSharp 1.22.4; project WaW.Common
- **Inputs:** `Content/Sound`
- **Outputs:** DLL. Native OpenAL is loaded by path from `runtimes/<rid>/native`.
- **Runtime:** OpenAL Soft
- **Used by:** WaWClient
- **Depends on:** WaW.Common
- **Migration destination:** **RETIRE.** Unity AudioSource replaces it. Keep the file-prefix music rules (`Menu_*`, `Realm_*`) as design data.

#### WaW.ContentBuilder (`WaW-Client/WaW.ContentBuilder/WaW.ContentBuilder.csproj`)
- **Name:** WaW.ContentBuilder. Builds to `ContentBuilder.exe`.
- **Purpose:** build-time tool that packs atlases, builds MSDF fonts, copies XML, sound and title assets, and loads FBX. It is driven by `Content/Content.xml`. 10 files, ~1.1k lines.
- **Language:** C#
- **Framework:** net10.0, Exe. Output goes to `bin\`.
- **Dependencies:**
  - NuGet: SharpAssimp 6.0.12, StbImageSharp 2.30.16, StbImageWriteSharp 1.16.7, StbRectPackSharp 1.0.4
  - Bundled `msdf-atlas-gen-w64.exe`
  - Project WaW.Common
- **Inputs:** `WaW-Client/WaWClient/Content/**`
- **Outputs:** `bin/.../Content`, and a hash-cached `Content/bin` (94 MB)
- **Runtime:** build time only, through the WaWClient PostBuild step. That step needs `$(SolutionDir)`, so building the single project fails.
- **Used by:** WaWClient build, `build_web.py` (reads its output), `deploy.ps1` (copies `Content/`)
- **Depends on:** none
- **Migration destination:** **RETIRE.** The Unity import pipeline (Sprite Atlas, TextMeshPro) replaces it. The source assets (PNG sheets, atlas definitions) must still be migrated.

#### WaW.ContentReader (`WaW-Client/WaW.ContentReader/WaW.ContentReader.csproj`)
- **Name:** WaW.ContentReader
- **Purpose:** reads the built atlases and fonts at runtime. 3 files.
- **Language:** C#
- **Framework:** net10.0
- **Dependencies:** projects WaW.Common, WaW.Engine
- **Inputs:** built `Content`
- **Outputs:** DLL
- **Runtime:** in-process
- **Used by:** UiLib, WaWClient, web
- **Depends on:** WaW.Engine
- **Migration destination:** **RETIRE.** If `Game.atlas` is kept as a data format, a Unity importer may be **rewritten later**.

#### WaW.ShaderSourceGen (`WaW-Client/WaW.ShaderSourceGen/WaW.ShaderSourceGen.csproj`)
- **Name:** WaW.ShaderSourceGen
- **Purpose:** Roslyn source generator that turns GLSL `AdditionalFiles` into C#.
- **Language:** C#
- **Framework:** netstandard2.0 Roslyn component
- **Dependencies:** Microsoft.CodeAnalysis.Analyzers 5.9.0, Microsoft.CodeAnalysis.CSharp 5.9.0, Microsoft.CodeAnalysis.CSharp.Workspaces 5.9.0
- **Inputs:** `*.vert`, `*.frag`
- **Outputs:** generated C#
- **Runtime:** compile time
- **Used by:** WaWClient, WaW.UiLib, web
- **Depends on:** none
- **Migration destination:** **RETIRE.** Unity shaders (HLSL/ShaderLab) are rewritten. Keep the GLSL as visual reference only.

#### WaWClient.Tests (`WaW-Client/Tests/WaWClient.Tests/WaWClient.Tests.csproj`)
- **Name:** WaWClient.Tests
- **Purpose:** xUnit tests for client rules: admin, app engine, combat, data, game, items, music, networking, rendering, roles. 35 files.
- **Language:** C#
- **Framework:** net10.0
- **Dependencies:** coverlet.collector 6.0.4, Microsoft.NET.Test.Sdk 17.14.1, xunit 2.9.3, xunit.runner.visualstudio 3.1.4; project WaWClient
- **Inputs:** client sources and Content
- **Outputs:** test results (`TestResults/` folders exist)
- **Runtime:** `dotnet test`
- **Used by:** `promote.ps1` (runs every test in the client solution)
- **Depends on:** WaWClient
- **Migration destination:** **Rewrite later** as Unity EditMode tests for the logic that is ported. Treat the current tests as a behavioural specification.

### 1.2 Shared

#### Common.Protocol (`Shared/Common.Protocol/Common.Protocol.csproj`)
- **Name:** Common.Protocol
- **Purpose:** rules that client and server must agree on:
  - `PacketId.cs` is the packet id table.
  - The other files are `LevelRules`, `WorldPosData`, `InventoryLayout`, `ItemCategories`, `Roles`, `WorldIds`, `Bounties`, `NexusCats` and `PowerUpList`.
  - 10 files, 539 lines.
- **Language:** C#
- **Framework:** net10.0, `Nullable` disabled
- **Dependencies:** none
- **Inputs:** none
- **Outputs:** DLL
- **Runtime:** in-process on both sides
- **Used by:** WaWClient, server `Common`, web (source-linked)
- **Depends on:** none
- **Migration destination:** **Split.**
  - Unity client: port it as C# (it is pure logic).
  - C++ GameServer: re-implement it in C++, or better, generate both sides from one schema.
  - Keep `ItemCategories` in sync with `Tools/Portal/build_portal_data.py`.

#### Common.Protocol.Tests (`Shared/Tests/Common.Protocol.Tests/Common.Protocol.Tests.csproj`)
- **Name:** Common.Protocol.Tests
- **Purpose:** tests for Bounties, ItemCategories, LevelRules, PacketId, Roles and WorldPosData.
- **Language:** C#
- **Framework:** net10.0 xUnit (same package versions as above)
- **Dependencies:** project Common.Protocol
- **Inputs:** none
- **Outputs:** results
- **Runtime:** `dotnet test`. It is listed in BOTH solutions.
- **Used by:** promote
- **Depends on:** Common.Protocol
- **Migration destination:** **Rewrite later.** Port to Unity tests and to C++ unit tests (for example Catch2 or GoogleTest). These are the parity tests between client and server.

### 1.3 Servers (`WaW-Server/`)

#### Common (server) (`WaW-Server/Common/Common.csproj`)
- **Name:** Common (server)
- **Purpose:** shared by both servers. 165 files, ~12.5k lines. Contains:
  - Database access (`Database/`): Npgsql and Dapper, `Schema.sql`, `DbWriter<T>`, `AccountLockManager` on Redis, `LoginTokens`, `PasswordHasher`, the rules classes.
  - The RPC contract and transport (`Messaging/`): StreamJsonRpc over TLS with a pinned self-signed certificate and a shared secret.
  - XML and world resources, config loaders, `OwnerSettings`, music, news, utilities.
- **Language:** C#
- **Framework:** net10.0; Release builds target x64; output `../bin/<cfg>`.
- **Dependencies:**
  - NuGet (purposes in DependencyMap section 2): Dapper 2.1.66, Ionic.Zlib.Core 1.0.0, Microsoft.Extensions.DependencyInjection.Abstractions 10.0.1 (no usage found), Newtonsoft.Json 13.0.3, Npgsql 9.0.3, StackExchange.Redis 2.8.31, StreamJsonRpc 2.25.29, System.Drawing.Common 8.0.4 (a dead `using System.Drawing;` in GameServer `User.cs`; no use found), System.Linq.Async 6.0.1 (usage UNVERIFIED).
  - Project: `Shared/Common.Protocol`.
- **Inputs:**
  - `Resources/Config/Data/*.xml`, listed in section 1.7.
  - `Resources/World/Data/*.jm` (9 maps) and `Config/*.json` (6 world configs).
  - `Resources/News/{News,PatchNotes}.txt`, `Resources/Rewards/DailyRewards.txt`, `Resources/Xml/Merchants/NexusMerchants.xml`.
  - **The 8 client XMLs linked from `WaW-Client/WaWClient/Content/Xmls/`.** They are copied to `Resources/Xml/Data/Xmls/` in the output.
- **Outputs:** DLL and resource files copied to `bin/`.
- **Runtime:** in-process in both servers.
- **Used by:** AccountServer, GameServer, Common.Tests
- **Depends on:** Postgres and Redis (only used from the AccountServer process); the client XMLs.
- **Migration destination:** **Split.**
  - Database, RPC hub and HTTP logic go to the **Account/API service (C#)**.
  - World, XML, map loading and game rules go to the **C++ GameServer**, re-implemented.
  - The resource files are **KEPT as data**.

#### AccountServer (`WaW-Server/AccountServer/AccountServer.csproj`)
- **Name:** AccountServer
- **Purpose:**
  - HTTP API on `HttpListener` (http.sys on Windows) bound to `<Address>:8080`. 27 files, ~1.8k lines. Its endpoints are listed in DependencyMap.md section 3.
  - RPC hub (`IpcServer`) on 8081 over TLS.
  - The **only process that talks to Postgres and Redis**.
- **Language:** C#
- **Framework:** net10.0 Exe; versioned with MinVer 7.0.0 (`v` tag prefix)
- **Dependencies:** Newtonsoft.Json 13.0.3, MinVer 7.0.0; project Common
- **Inputs:** config XMLs (`appEngineConfig`, `postgresConfig`, `redisConfig`, `rpcServerConfig`, `newAccountsConfig`, `newsConfig`, `musicConfig`); `/opt/WaW/settings/*` (owner settings) on the VPS; `Schema.sql`, run at every start.
- **Outputs:**
  - Postgres rows and Redis lock keys.
  - `rpc-server.pfx` and `rpc-server.cer`, created on first start.
  - Logs on stdout, read through journald.
- **Runtime:** Linux systemd service `alloy-account` (`dotnet AccountServer.dll`, working directory `/opt/alloy-server`).
- **Used by:** WaWClient, the web client (through nginx `/api/`), Launcher, Installer (no; it only reads the manifest), DeveloperDashboard, Portal site, E2E tests, GameServer (RPC).
- **Depends on:** Postgres 5432, Redis 6379.
- **Migration destination:** **Account/API service (C#).** Keep the HTTP contract, including `/public/*`, `/dev/*` and `/account/remember`, because the Launcher, Dashboard and Portal depend on it. Replace `HttpListener` with ASP.NET Core and consider JSON instead of XML responses. The RPC hub must become a protocol the C++ GameServer can speak; see DependencyMap.md.

#### GameServer (`WaW-Server/GameServer/GameServer.csproj`)
- **Name:** GameServer
- **Purpose:** the authoritative simulation. 210 files, ~15k lines. Covers:
  - A TCP listener on 2050 and a single-threaded game loop (20 TPS).
  - Worlds and ECS-style managers, behaviours, anti-cheat, loot and quests.
- **Language:** C#
- **Framework:** net10.0 Exe; MinVer 7.0.0. With the optional `-p:BehaviorHotReload=true` it also references Microsoft.CodeAnalysis.Common / CSharp 4.14.0.
- **Dependencies:** project Common
- **Inputs:** `gameServerConfig.xml` (Port, Address, Version, TPS, MaxPlayers, MaxClientsPerIP), `gameConfig.xml`, `realmConfig.xml`, `serverSettings.xml` (owner), `rpcClientConfig.xml` and `rpc-server.cer`, the XMLs and the world maps.
- **Outputs:** game packets; RPC calls (save, death, moderation and others); `[STATS]` logs.
- **Runtime:** systemd service `alloy-game` (`ExecStartPre` sleeps 6 s so the AccountServer can create the certificate first).
- **Used by:** WaWClient (TCP); the web client (through ws-bridge); E2E tests.
- **Depends on:** AccountServer RPC 8081. It has **no direct database or Redis access** (verified: no `DbClient` or Redis use under `GameServer/`).
- **Migration destination:** **C++ GameServer.** Re-implement. Keep the wire protocol, or version it deliberately. Use the GameServer.Tests and E2E scripts as the behavioural specification.

#### Common.Tests (`WaW-Server/Tests/Common.Tests/Common.Tests.csproj`)
- **Name:** Common.Tests
- **Purpose:** rules and DB-layer logic tests: attempt limiter, bug board, campsite, character DB, slots, daily rewards, dashboard, fame, gift chest, login tokens, moderation, music, owner settings, password hasher, patch notes, public profile, ranks, rewards, stat rules, structs, utilities. 25 files.
- **Language:** C#
- **Framework:** net10.0 xUnit
- **Dependencies:** project Common
- **Inputs:** none
- **Outputs:** results
- **Runtime:** `dotnet test`. Whether it needs a live database is UNVERIFIED; it appears not to.
- **Used by:** promote
- **Depends on:** Common
- **Migration destination:** **Account/API service (C#).** Port the account-side tests (mostly reusable as C#). Gameplay-rule tests (StatRules, FameRules) also need C++ equivalents.

#### GameServer.Tests (`WaW-Server/Tests/GameServer.Tests/GameServer.Tests.csproj`)
- **Name:** GameServer.Tests
- **Purpose:** behaviours, combat, commands, items, network, persistence, session, weather and worlds. `TestWorldFactory` builds a real `World` without a database. 43 files.
- **Language:** C#
- **Framework:** net10.0 xUnit
- **Dependencies:** project GameServer
- **Inputs:** server resources
- **Outputs:** results
- **Runtime:** `dotnet test`
- **Used by:** promote
- **Depends on:** GameServer
- **Migration destination:** **C++ GameServer** tests. Rewrite in C++. This is the best behavioural specification in REF.

### 1.4 Browser client (`WebClient/`)

#### web / WarriorsWeb (`WebClient/web/web.csproj`)
- **Name:** WarriorsWeb (`Microsoft.NET.Sdk.WebAssembly`)
- **Purpose:** the same client sources compiled to WASM.
  - Native parts are swapped for shims in `web/shim` (GL to WebGL2 through `wwwroot/gl.js`, Platform, Audio, `WebClient.cs` for WebSocket instead of TCP, `ReFuel.cs`, `StorageBuffer`, `Enums.g.cs`).
  - `patched.props` removes 9 desktop files that `tools/patch_sources.py` replaces with patched copies.
- **Language:** C# and JavaScript
- **Framework:** net10.0 WebAssembly. Interpreter by default; AOT with `WwAot=true` (needs the `wasm-tools` workload in `%USERPROFILE%\.dotnet-wasm`).
- **Dependencies:**
  - NuGet: OpenTK.Mathematics 5.0.0-pre.16, Microsoft.Extensions.Logging 10.0.11, Microsoft.Extensions.Logging.Console 10.0.11, BouncyCastle.Cryptography 2.7.0, NLayer 1.16.0, StbVorbisSharp 1.22.4, StbImageSharp 2.30.16.
  - ShaderSourceGen as an analyzer.
  - Source-linked: WaW.Common, ContentReader, Engine, UiLib, WaWClient, Common.Protocol.
- **Inputs:** desktop sources; `web/shaders` (generated by `port_shaders.py`); desktop built `Content` (`WaW-Client/WaWClient/bin/Debug/net10.0/Content`).
- **Outputs:** `WebClient/dist/site` or `site-aot` (index.html, `_framework`, content, `content.json`)
- **Runtime:** a browser served by nginx at `play.<domain>`. It calls `/api` (AccountServer) and `/game` (ws-bridge).
- **Used by:** browser players
- **Depends on:** nginx, ws-bridge, AccountServer
- **Migration destination:** **RETIRE.** A Unity WebGL build is the replacement, if a browser version is still wanted (decision needed). Unity WebGL cannot open raw TCP, so the WebSocket path, through the bridge or native WebSocket in the C++ server, must stay.

#### enumgen (`WebClient/tools/enumgen/enumgen.csproj`)
- **Name:** enumgen
- **Purpose:** emits `web/shim/Enums.g.cs`, copies of the OpenTK GL and Platform enums the client uses.
- **Language:** C#
- **Framework:** net10.0 Exe
- **Dependencies:** OpenTK 5.0.0-pre.16
- **Inputs:** source roots
- **Outputs:** `Enums.g.cs`
- **Runtime:** developer tool
- **Used by:** the web build workflow (run by hand)
- **Depends on:** none
- **Migration destination:** **RETIRE.**

#### WebClient Python tools (`WebClient/tools/*.py`, `rebuild.sh`, `acts_*.json`)
- **Name:** web build and test tools

  | Tool | What it does |
  | --- | --- |
  | `build_web.py` | Patch, port shaders, `dotnet publish`, copy content, write `content.json` |
  | `patch_sources.py`, `patch_rules_more.py` | Text patches to desktop files, to `web/patched` |
  | `port_shaders.py` | GLSL 330 to GLSL ES 3.00 |
  | `shadercheck.py` | Playwright and headless Chrome WebGL2 compile check |
  | `webtest.py`, `acts_*.json` | Headless browser test runner and its action scripts |
  | `serve.py` | Local static server that proxies `/api` |
  | `ws_bridge.py` | Local WebSocket-to-TCP bridge |
  | `errs.py` | Build-log error de-duplication |
  | `rebuild.sh` | Rebuild loop that hard-codes an old Claude scratchpad path |

- **Language:** Python 3 (Playwright, websockets, PIL not needed), bash
- **Framework:** none
- **Dependencies:** dotnet SDK, Chrome
- **Inputs:** sources and content
- **Outputs:** web site build
- **Runtime:** developer PC
- **Used by:** `deploy.ps1 -Web`
- **Depends on:** desktop client build
- **Migration destination:** **RETIRE.** The behaviour flows in `acts_*.json` (login, register, enter, play, persist) are useful as a browser E2E reference.

#### WebClient VPS files (`WebClient/vps/setup_web.sh`, `ws_bridge.py`, `patch_cache.py`)
- **Name:** `play.<domain>` hosting
- **Purpose:**
  - `setup_web.sh` installs nginx, certbot and python3-venv.
  - It creates the `ww-bridge` systemd service (`ws_bridge.py 2051 127.0.0.1 2050`) and writes `/etc/nginx/conf.d/warriors.conf`:
    - `/` static files from `/var/www/warriors`
    - `/download/` (client, launcher and manifest archives)
    - `/api/` proxied to `http://<VPS host address>:8080/` with a `Host` header rewrite
    - `/game` WebSocket proxied to 127.0.0.1:2051
  - It runs certbot.
  - `patch_cache.py` is a one-off nginx cache fix.
- **Language:** bash, Python
- **Framework:** nginx, websockets (pip, in a venv)
- **Dependencies:** apt
- **Inputs:** domain argument
- **Outputs:** nginx config, systemd unit, TLS certificate
- **Runtime:** VPS (root)
- **Used by:** `deploy.ps1 -SetupWeb`
- **Depends on:** DNS A record
- **Migration destination:**
  - **REPLACE** with a declarative reverse-proxy config (nginx or Caddy) kept in the new repo.
  - Keep the `/download/` static hosting and the `/api/` proxy.
  - The bridge stays only while a WebGL client exists.

### 1.5 Launcher, installer, dashboard

#### Launcher (`Launcher/Launcher.csproj`)
- **Name:** WaWLauncher, version 3.0.3 (`<Version>` must equal `SelfUpdate.Version`).
- **Purpose:** sign-in, check and update the game from the manifest, self-update, start the game with a token, uninstall. Details in section 2.
- **Language:** C#; UI built in code (no XAML)
- **Framework:** net10.0 WinExe, Avalonia 12.1.3. Published self-contained, single-file, trimmed (`TrimMode` partial), for win-x64, linux-x64, osx-arm64 and osx-x64.
- **Dependencies:** Avalonia.Desktop 12.1.3, Avalonia.Fonts.Inter 12.1.3, Avalonia.Themes.Fluent 12.1.3, System.Security.Cryptography.ProtectedData 10.0.0
- **Inputs:** `https://play.<domain>/download/manifest.json` and archives; `https://play.<domain>/api/account/*`; `.wawlauncher.json`; `%LocalAppData%\WaWLauncher\login.json`
- **Outputs:** `<install>/Game/`; the HKCU Uninstall registry entry; shortcuts (`.lnk` / `.desktop`); the started game process
- **Runtime:** player PC
- **Used by:** players; the Installer (links its files); DeveloperDashboard (links `UiKit.cs`, `EmberField.cs`, assets)
- **Depends on:** nginx `/download/` and `/api/` on the VPS
- **Migration destination:** **KEEP, with adaptation.** It is engine-agnostic: it downloads an archive and starts an executable with environment variables. Needed changes:
  - Unity build layout (exe plus `_Data` plus `UnityPlayer.dll`).
  - Game file name.
  - The Unity client must read `WAW_LAUNCH_USER` / `WAW_LAUNCH_TOKEN`.
  - Option: move it to its own repo.

#### Launcher.Tests (`Launcher/Tests/Launcher.Tests/Launcher.Tests.csproj`)
- **Name:** Launcher.Tests
- **Purpose:** 24 tests: manifest parse (including BOM), platform pick, install / swap / migration, self-update naming, uninstall, saved login, version constant, installer placement (`extern alias installer`).
- **Language:** C#
- **Framework:** net10.0 xUnit
- **Dependencies:** projects Launcher, Installer (`Aliases="installer"`)
- **Inputs:** none
- **Outputs:** results
- **Runtime:** `dotnet test`. **Not run by promote.**
- **Used by:** developer only
- **Depends on:** Launcher, Installer
- **Migration destination:** **KEEP as-is**, together with the Launcher.

#### Installer (`Installer/Installer.csproj`)
- **Name:** WaW-Installer, version 1.0.1
- **Purpose:** what the website hands out. It does the following:
  1. Choose the folder and shortcuts.
  2. Download the launcher's self-update archive named in the manifest, check its SHA-256 and place it as `WaWLauncher(.exe)`.
  3. Save `.wawlauncher.json`, create shortcuts and write the Windows Apps entry.
  4. Optionally open the launcher.
- **Language:** C#
- **Framework:** net10.0 WinExe, Avalonia 12.1.3, single-file and trimmed; win-x64 and linux-x64 only (no Mac installer)
- **Dependencies:** the three Avalonia 12.1.3 packages. It **links** `Launcher/UiKit.cs`, `EmberField.cs`, `Install.cs`, `Shortcuts.cs`, `Updater.cs`, `Manifest.cs`, `AppsEntry.cs` and the assets.
- **Inputs:** `manifest.json`, the launcher archive
- **Outputs:** install folder, shortcuts, registry entry
- **Runtime:** player PC
- **Used by:** the website download page
- **Depends on:** `/download/` on the VPS
- **Migration destination:** **KEEP as-is**, tied to the Launcher. Later this could be **REPLACED** by a standard packager (MSIX, Inno Setup or similar).

#### DeveloperDashboard (`DeveloperDashboard/DeveloperDashboard.csproj`)
- **Name:** WaW-DeveloperDashboard, version 1.0.0
- **Purpose:** staff-only tool (rank Developer 90+). Pages: Status, Players (search, kick, mute, ban, rank, mail), Patch Notes (post / delete), Anti-cheat list, Weather.
- **Language:** C#
- **Framework:** net10.0 WinExe, Avalonia 12.1.3, single-file
- **Dependencies:** Avalonia 12.1.3 (3 packages), System.Security.Cryptography.ProtectedData 10.0.0; links the Launcher UI kit
- **Inputs:** server choice (Live is `https://play.<domain>/api/`, Local is `http://127.0.0.1:8080/`); username and password sent once to `/account/remember`; the DPAPI-saved token
- **Outputs:** `/dev/*` POSTs (JSON answers)
- **Runtime:** staff PC. **No deploy step publishes it.**
- **Used by:** staff
- **Depends on:** AccountServer `/dev/*`. Some of these fan out over RPC to the GameServer (`GetStatus`, `ApplyModeration`, `GetWeather` / `SetWeather`).
- **Migration destination:** **KEEP as-is**, provided the Account/API service keeps the `/dev/*` contract and the C++ GameServer implements the matching RPC methods. A web admin page could later **REPLACE** it.

#### DeveloperDashboard.Tests (`DeveloperDashboard/Tests/DeveloperDashboard.Tests/DeveloperDashboard.Tests.csproj`)
- **Name:** DeveloperDashboard.Tests
- **Purpose:** 10 tests: news bullet rules, no Owner rank offered, API answer parsing, saved session, unknown-server fallback.
- **Language:** C#
- **Framework:** net10.0 xUnit
- **Dependencies:** project DeveloperDashboard
- **Inputs:** none
- **Outputs:** results
- **Runtime:** `dotnet test`. Not run by promote.
- **Used by:** developer only
- **Depends on:** DeveloperDashboard
- **Migration destination:** **KEEP as-is**, together with the dashboard.

### 1.6 Portal and Forums

#### Portal site (`Portal/site/`)
- **Name:** The Portal
- **Purpose:** public player, guild, leaderboard and wiki site. Pages: `index`, `player`, `guild`, `leaderboards`, `items`, `classes`, `graveyard` (static; "the game does not record deaths yet") and `releases` (a redirect to the website's patch notes since 2026-10-01).
- **Language:** HTML, CSS, vanilla JS (`js/portal.js`, 363 lines, one script for every page, picked by `<body data-page>`)
- **Framework:** none
- **Dependencies:** `data/*.json` and `icons/` (generated)
- **Inputs:** `/api/public/{online,search,player,guild,leaderboard}`; `data/items.json`, `data/classes.json`, `data/build.json` (95 items, 2 classes, game version 0.3.16)
- **Outputs:** web pages
- **Runtime:** nginx at `portal.<domain>`, root `/var/www/portal`
- **Used by:** public visitors; launchers up to 3.0.3 (`releases.html`)
- **Depends on:** the AccountServer `/public/*` API (JSON); `Tools/Portal/build_portal_data.py`
- **Migration destination:** **KEEP as-is.** It depends only on the `/public/*` JSON contract and on generated data. The Account/API service must keep `/public/*`.

#### Portal VPS setup (`Portal/vps/setup_portal.sh`)
- **Name:** setup_portal.sh
- **Purpose:** nginx site `/etc/nginx/sites-available/portal`:
  - Pretty URLs: `/player/*`, `/guild/*`, `/top/*`, `/wiki/*`, `/graveyard`.
  - `/api/public/` proxied to `http://<ip>:8080/public/` (with the `Host` rewrite); every other `/api/` path returns 404.
  - certbot for TLS.
- **Language:** bash
- **Framework:** nginx
- **Dependencies:** apt
- **Inputs:** domain, IP
- **Outputs:** nginx config, TLS certificate
- **Runtime:** VPS
- **Used by:** `deploy -SetupPortal`
- **Depends on:** DNS
- **Migration destination:** **REPLACE** with versioned reverse-proxy config (same routes).

#### Forums (`Forums/vps/setup_forums.sh`)
- **Name:** NodeBB forums
- **Purpose:** installs Node 20 (NodeSource) and NodeBB v3.x in `/opt/nodebb` (systemd service `ww-forums` on 127.0.0.1:4567), a `nodebb` Postgres role and database on the same Postgres, nginx with certbot, and a 2 GB swap file if none exists. Re-running it upgrades NodeBB.
- **Language:** bash (with an embedded Python JSON writer)
- **Framework:** NodeBB, Node.js 20
- **Dependencies:** apt, git, npm
- **Inputs:** domain, admin user, email, password (prompted by `deploy.ps1`)
- **Outputs:** running forum; `/opt/nodebb/config.json` (holds the generated DB password)
- **Runtime:** VPS
- **Used by:** `deploy -SetupForums`
- **Depends on:** Postgres
- **Migration destination:** **KEEP as-is**, or replace it with a hosted forum or Discord. It is unrelated to the game code. Do not move it onto the game database host in the new design unless resources allow.

### 1.7 Config groups

#### Server config XMLs (`WaW-Server/Common/Resources/Config/Data/`)
- **Name:** server configs
- **Purpose:**

  | File | Contents |
  | --- | --- |
  | `appEngineConfig.xml` | HTTP port 8080, Address, MaxConcurrentRequests, DownloadUrl |
  | `gameServerConfig.xml` | Port 2050, Address, **Version 0.3.16**, TPS 20, MaxPlayers, MaxClientsPerIP, BehaviorsDir |
  | `rpcServerConfig.xml` | Listen 0.0.0.0:8081, PFX path, certificate password, shared secret |
  | `rpcClientConfig.xml` | Host 127.0.0.1:8081, trusted `.cer`, shared secret |
  | `postgresConfig.xml` | Connection settings |
  | `redisConfig.xml` | localhost:6379 |
  | `realmConfig.xml` | Upstream realm names and events (they name archived bosses) |
  | `serverSettings.xml` | Owner overrides |
  | Others | `gameConfig.xml`, `newAccountsConfig.xml`, `newCharsConfig.xml`, `newsConfig.xml`, `musicConfig.xml`, and the `*.example.xml` files |

- **Language:** XML
- **Framework:** loaded by `Common/Resources/Config/*.cs` (`ConfigLoader` hot-reloads every 2 s)
- **Dependencies:** none
- **Inputs:** hand-edited
- **Outputs:** none
- **Runtime:** both servers
- **Used by:** both servers; `deploy.ps1` (rewrites `<Address>` in the packaged copy and drops the machine-owned files); `promote.ps1` (reads the version)
- **Depends on:** none
- **Migration destination:** **Rewrite later**, split by service:
  - The Account/API service uses ASP.NET configuration with environment variables and secrets.
  - The C++ GameServer uses its own config file.
  - **Note:** `rpcServerConfig.xml`, `rpcClientConfig.xml` and `postgresConfig.xml` contain real secrets (the RPC certificate password and shared secret, and the DB settings). They are git-ignored but present in REF. Values are not reproduced here. Rotate them before any reuse.

#### Owner settings (`/opt/WaW/settings/` on the VPS, `OwnerSettings.cs`)
- **Name:** owner settings
- **Purpose:** hot-reloaded owner overrides that deploys never touch: `daily-rewards.txt`, `new-accounts.xml`, `new-characters.xml`, `game.xml`, `server.xml`. On Windows they are used only if `WAW_SETTINGS_DIR` is set.
- **Language:** XML, text
- **Framework:** none
- **Dependencies:** none
- **Inputs:** owner edits
- **Outputs:** none
- **Runtime:** VPS
- **Used by:** both servers
- **Depends on:** none
- **Migration destination:** **Rewrite later.** Live-ops settings belong in the Account/API service DB or config, plus a GameServer config reload.

#### World data (`WaW-Server/Common/Resources/World/Data/*.jm`, `Config/*.json`)
- **Name:** maps and world configs
- **Purpose:**
  - 9 `.jm` maps: Campsite, Guild0-3, Initiation, Nexus, Realm, TestingWorld. The format is JSON with a base64 + zlib int16 tile index.
  - 6 world configs: zones, music, Blocksight and similar.
  - **They must be listed in `Common.csproj`** or they do not reach the server package.
- **Language:** JSON
- **Framework:** none
- **Dependencies:** Ionic.Zlib (decoding)
- **Inputs:** the map editor
- **Outputs:** none
- **Runtime:** GameServer
- **Used by:** GameServer, `Tools/Editor`, `preview_map.py`
- **Depends on:** the XML type ids
- **Migration destination:** **KEEP as data.** The C++ GameServer needs a `.jm` loader. The Unity client receives map data over the wire (UNVERIFIED whether the client ever reads `.jm` files directly; none were found under client Content).

#### Shared content XML (`WaW-Client/WaWClient/Content/Xmls/*.xml`)
- **Name:** game content definitions
- **Purpose:** Containers, Equip, Ground, NPCs, Objects, Players, Projectiles, StaticObjects (~230 KB). This is the single source of truth for types, items, classes and objects.
- **Language:** XML
- **Framework:** none
- **Dependencies:** none
- **Inputs:** hand-edited ("everything is hand-edited source now", `CLAUDE.md` 2026-10-02)
- **Outputs:** copied into the client `Content` and linked into the server
- **Runtime:** client and server
- **Used by:** WaWClient, the server (through `Common.csproj` `<Content Link>`), `build_portal_data.py`, `build_palette.py`, `preview_map.py`, the web build (from the built Content)
- **Depends on:** atlas names
- **Migration destination:** **KEEP as data.** It moves to a neutral `/content` folder consumed by the Unity importer, the C++ loader and the Portal tool. See DependencyMap.md section 5.

### 1.8 Database and cache components

#### PostgreSQL schema (`WaW-Server/Common/Database/Schema.sql`, 154 lines)
- **Name:** `alloy` database
- **Purpose:** 13 tables: `accounts` (JSONB `data`: the whole account including characters and vault), `logins`, `guilds`, `bans`, `mutes`, `bug_posts`, `inbox_messages`, `daily_rewards`, `login_tokens`, `anticheat_flags`, `gift_items`, `gift_chests`, `patch_notes`. The script is idempotent and runs at every AccountServer start.
- **Language:** SQL
- **Framework:** PostgreSQL 17 (local, per README); the VPS gets the apt default version (UNVERIFIED which)
- **Dependencies:** none
- **Inputs:** none
- **Outputs:** tables
- **Runtime:** systemd `postgresql`, localhost:5432
- **Used by:** AccountServer only; NodeBB (separate `nodebb` database)
- **Depends on:** none
- **Migration destination:** **Account/API service (C#)** owns it. Redesign later:
  - Normalise characters out of the account JSONB.
  - Use real migrations (EF Core migrations or a tool such as DbUp or Flyway) instead of a script run at start-up.

#### Redis
- **Name:** Redis (Memurai on Windows)
- **Purpose:** **only** account locks (`AccountLockManager`, which stops one account being online on two servers). Stale locks are released at AccountServer start.
- **Language:** none
- **Framework:** StackExchange.Redis 2.8.31
- **Dependencies:** none
- **Inputs:** none
- **Outputs:** lock keys
- **Runtime:** systemd `redis-server`, localhost:6379
- **Used by:** AccountServer only
- **Depends on:** none
- **Migration destination:** **Account/API service (C#)**, which keeps using it for sessions and locks. Do not add more Redis uses early; see Deployment.

### 1.9 Tools (`Tools/`)

The KEEP / REWRITE / REPLACE / RETIRE verdicts are in section 5.

#### Tools/Editor
- **Name:** W&W Editor
- **Purpose:**
  - `editor.html` and `editor.js` are a browser map editor for `.jm` files, with a Zones tab and an item / object maker that writes XML.
  - `build_palette.py` writes `palette.js`.
  - `preview_map.py` renders a map to PNG offline.
  - `open_editor.cmd` opens the editor in a browser.
  - `tests/run_tests.py` runs the editor's JavaScript in headless Chrome or Edge against the real maps.
- **Language:** JavaScript, Python (PIL), cmd
- **Framework:** none
- **Dependencies:** Chrome or Edge, Python 3, Pillow
- **Inputs:** client XMLs, `Game.atlas` and sheets, `WaW-Server/Common/Resources/World/MapData.cs` (region list), world configs and `.jm` maps
- **Outputs:** `.jm` maps, world-config zones, XML snippets, `palette.js`, PNG previews
- **Runtime:** developer PC
- **Used by:** the content author
- **Depends on:** the content formats
- **Migration destination:** **KEEP** while `.jm` and XML stay the content formats.

#### Tools/Portal/build_portal_data.py
- **Name:** Portal data builder
- **Purpose:** writes `Portal/site/data/{items,classes,build}.json` and `icons/items|classes/<hex>.png` (x3 nearest-neighbour).
- **Language:** Python (PIL)
- **Framework:** none
- **Dependencies:** none
- **Inputs:** `Content/Xmls/*.xml`, `Content/Game.atlas`, `Content/Sheets/*.png`, `Core/Settings.cs` (BuildVersion)
- **Outputs:** Portal data
- **Runtime:** developer PC (run by `deploy -Portal`)
- **Used by:** Portal
- **Depends on:** XML and atlas formats; it must stay in sync with `Shared/Common.Protocol/ItemCategories.cs`
- **Migration destination:** **KEEP.** Change it only if the art or atlas pipeline changes in Unity, and read the version from the new location.

#### Tools/Mac/build_mac_app.py
- **Name:** Mac app builder
- **Purpose:** builds `WaW-Mac.zip`, the "Warriors & Wizards.app" bundle that wraps both chips' launchers. It writes `Info.plist`, a `launch` script, `AppIcon.icns` and Unix modes.
- **Language:** Python (PIL)
- **Framework:** none
- **Dependencies:** none
- **Inputs:** launcher osx publish folders, version
- **Outputs:** `dist/WaW-Mac.zip`
- **Runtime:** developer PC (run by `deploy -Launcher`)
- **Used by:** Mac players
- **Depends on:** the Launcher
- **Migration destination:** **KEEP** while the Launcher is kept. Untested on a real Mac (`CLAUDE.md`).

#### Tools/Admin
- **Name:** character reset
- **Purpose:** `reset_characters.sql` clears `Characters`, sets `NextCharId` to 0 and `StarterPending` to true in every `accounts.data` row. The `_local.ps1` / `.cmd` pair backs up the accounts table locally and runs the script. The `_vps.ps1` / `.cmd` pair stops the services, backs up on the VPS, runs it and restarts.
- **Language:** SQL, PowerShell, cmd
- **Framework:** psql, pg_dump
- **Dependencies:** PostgreSQL 17 binaries (local path is hard-coded); the VPS host address (hard-coded)
- **Inputs:** none
- **Outputs:** backup `.sql` / `.sql.gz`
- **Runtime:** developer PC or VPS
- **Used by:** owner
- **Depends on:** the accounts JSONB layout
- **Migration destination:** **REWRITE.** It is schema-bound; move it to a proper admin endpoint or migration script.

#### Tools/E2E
- **Name:** live protocol tests
- **Purpose:** see section 7.2.
- **Language:** Python (stdlib only: socket, struct, urllib)
- **Framework:** none
- **Dependencies:** none
- **Inputs:** local servers (127.0.0.1:2050 and :8080); `Core/Settings.cs` (version)
- **Outputs:** PASS / FAIL lines; throwaway `Rt*` accounts in the local DB
- **Runtime:** developer PC
- **Used by:** developer
- **Depends on:** the wire protocol and the HTTP API
- **Migration destination:** **KEEP** as the behavioural regression harness for the C++ GameServer and the API service, while the wire protocol is kept.

### 1.10 Release and deployment scripts

#### deploy.ps1 / deploy.cmd
- **Name:** deploy
- **Purpose:** build, package, upload to the VPS, restart services; VPS care commands. The flags are in section 6.
- **Language:** PowerShell 5.1 (the `.cmd` wrapper runs it with `-ExecutionPolicy Bypass`)
- **Framework:** dotnet CLI, robocopy, tar, ssh/scp (password or key), Python
- **Dependencies:** OpenSSH client, Python 3, .NET 10 SDK, optionally the wasm SDK
- **Inputs:** the repo; `-VpsHost` (default: the VPS host address); `-VpsUser root`; domains
- **Outputs:** `dist/*`; files on the VPS (`/opt/alloy-server`, `/var/www/warriors`, `/var/www/portal`, `download/manifest.json`)
- **Runtime:** developer PC
- **Used by:** the owner
- **Depends on:** VPS SSH as root
- **Migration destination:** **REPLACE** with CI-built artifacts and a small, reviewed deploy script or pipeline. See the recommendation in section 6.

#### promote.ps1 / promote.cmd
- **Name:** promote
- **Purpose:** mirror the "Testing" folder into the "Game" folder. The default is a dry run. `-Apply` builds both solutions, runs their tests, zips the target to `Documents\WW_Backups` and runs `robocopy /MIR` (excluding `.git`, `bin`, `obj`, `dist` and similar).
- **Language:** PowerShell
- **Framework:** robocopy, Windows `tar.exe`
- **Dependencies:** none
- **Inputs:** `-Target` (default `C:\Users\cbart\Desktop\Repos\Warriors-and-Wizards-Game`)
- **Outputs:** mirrored folder and backup zip
- **Runtime:** developer PC
- **Used by:** owner
- **Depends on:** the target having `.git`
- **Migration destination:** **RETIRE.** Git branches, tags and CI replace the two-folder scheme.

#### VPS_SETUP.md
- **Name:** VPS runbook
- **Purpose:** manual steps:
  1. Install Postgres, Redis and the .NET 10 runtime with apt.
  2. Create the `alloy` database.
  3. Set up the ufw firewall (22, 8080, 2050; plus 80/443 added by the setup scripts).
  4. Create systemd units `alloy-account` and `alloy-game`.
  5. Upload the first server build with scp.

  It also describes the website, Portal, forums, launcher and manifest.
- **Language:** Markdown
- **Framework:** none
- **Dependencies:** none
- **Inputs:** none
- **Outputs:** none
- **Runtime:** VPS
- **Used by:** owner
- **Depends on:** none
- **Migration destination:** **Rewrite later** as infrastructure documentation for the new stack.

### 1.11 Docs and other folders

| Item | Purpose | Migration destination |
| --- | --- | --- |
| `README.md` | Overview, build, run, release (partly stale; see Discrepancies) | Rewrite later |
| `CLAUDE.md` | 128 KB session rulebook. Its hardware constraints, protocol traps and the rules for "stat numbers", "string stats in three places" and "PlayerShoot numbering" are valuable specification. Its tool and asset paths are history. | Mine for requirements; then RETIRE |
| `Docs/EngineeringAudit.md` | Architecture, findings, change log, test list (2026-09-21, partly stale) | Reference only |
| `Docs/ProjectHistory.md` | Session log | Reference only |
| `Docs/3D_OBJECT_PHILOSOPHY.md` | Art direction for 3D props (3D was removed 2026-10-01) | RETIRE |
| `WaW-Server/AGENTS.md` | Server rules (rewritten 2026-09-21; mostly accurate) | Reference for the C++ port |
| `WaW-Server/README.md`, `WaW-Client/README.md` | Upstream Alloy READMEs (credits, licences) | Keep the credits and licences |
| `Archive/ServerBehaviors` | 17 upstream behaviour scripts (~14.9k lines) driving enemies that do not exist in W&W data | RETIRE (reference only) |
| `ExtraAssets/` | Unused music, old 32 px heroes | RETIRE, or move to the asset archive |
| `Campsite_preview.png` | Loose preview image | RETIRE |
| `dist/`, `WebClient/dist/` | Build output (3.2 GB together). It includes stale names no longer uploaded: `alloy-server.tgz`, `WarriorsAndWizards-Setup.exe`, `WaWLauncher.exe`, `WaWLauncher-linux.tar.gz`. | Do not migrate |

---

## 2. Launcher, in depth (`Launcher/`)

**What it does.** Since 3.0.0 (2026-09-24) it is only a launcher; the installer makes the folder. On start it works through these steps:
1. `CleanupOld()` removes `<exe>.old`.
2. On a Mac it switches the install dir to `~/Library/Application Support/Warriors and Wizards` and creates the state file.
3. If it is running under the legacy 1.0.0 name `WarriorsAndWizards(.exe)`, it copies itself to `WaWLauncher`, starts that copy with `--migrate-from <old> --pid <n>` and exits. `Legacy.Cleanup` then removes the old launcher and the `game`, `game.new` and `game.old` folders.
4. Otherwise it opens the window (`LauncherWindow`). Command-line modes:

| Argument | Effect |
| --- | --- |
| `--check` | Report only; headless |
| `--no-launch` | Check and install, no window, no game start |
| `--uninstall` | Opens the remove page |
| `--just-updated`, `--migrate-from`, `--pid` | Internal |

**Install layout** (`Install.cs Layout`):

| Item | Meaning |
| --- | --- |
| `WaWLauncher(.exe)` | The launcher |
| `Game/` | `WarriorsAndWizards(.exe)`, `Content/`, `runtimes/...`. Replaced as a whole. |
| `.wawlauncher.json` | Hidden on Windows. Holds `gameVersion`, `gameFiles` (2.x legacy), `desktopShortcut`, `menuShortcut`, `closeOnPlay` |
| `.waw-staging`, `.waw-old` | Swap folders |

- 2.x installs are moved into `Game/` by `MoveIntoGameFolder`.
- The old game name `AlloyClient` is still recognised.
- Default install dirs: Windows `%LocalAppData%\Programs\Warriors & Wizards`; Linux `~/.local/share/warriors-and-wizards`; Mac Application Support.

**Version handling.**
- The game is updated when `Versions.Differ(installed, manifest.version)`. This is a string comparison, so any difference counts, and **downgrades are possible**. That is intended: the server version is authoritative.
- The launcher self-updates only when `Versions.IsNewer(manifest.launcher.version, SelfUpdate.Version)`, using `System.Version` numeric comparison. It never downgrades.
- `SelfUpdate.Version` = "3.0.3" must equal `<Version>` in `Launcher.csproj`; the test `TheLauncherKnowsItsOwnPublishedVersion` enforces this.
- After a self-update restart, `skipLauncherUpdate` stops restart loops.
- The game client and the servers must have identical versions. The GameServer refuses other clients (`gameServerConfig.xml <Version>` = `Settings.BuildVersion` = 0.3.16).

**manifest.json format.** `deploy.ps1 Update-Manifest` writes it to `/var/www/warriors/download/manifest.json` as UTF-8 without a BOM; the parser tolerates a BOM. Sample shape (from `dist/manifest.json`):
```json
{
  "version": "0.3.16",
  "windows":  { "file": "WarriorsAndWizards-Client.zip",            "size": 147961759, "sha256": "<hex>" },
  "linux":    { "file": "WarriorsAndWizards-Client-linux.tar.gz",   "size": ..., "sha256": "<hex>" },
  "macArm64": { "file": "WarriorsAndWizards-Client-mac-arm64.tar.gz", ... },
  "macX64":   { "file": "WarriorsAndWizards-Client-mac-x64.tar.gz",   ... },
  "launcher": { "version": "3.0.3",
                "windows":  { "file": "WarriorsAndWizards-Launcher-windows.zip", ... },
                "linux":    { "file": "WarriorsAndWizards-Launcher-linux.tar.gz", ... },
                "macArm64": { "file": "WarriorsAndWizards-Launcher-mac-arm64.tar.gz", ... },
                "macX64":   { "file": "WarriorsAndWizards-Launcher-mac-x64.tar.gz", ... } }
}
```
- The file names are relative to the download base `https://play.warriorsandwizards.com/download/`. In DEBUG builds `WAW_DOWNLOAD_BASE` overrides it.
- JSON is read with source-generated `System.Text.Json`, so trimming is safe.
- **There is no signature on the manifest.** Integrity relies on HTTPS plus the SHA-256 values listed inside the same manifest.

**Download and update process** (`Engine.cs`, `Updater.cs`, `Install.cs`):
1. `GET manifest.json?t=<unix>` (cache-buster) with a 15 s timeout. If it fails while a game is installed, the result is ReadyOffline.
2. If a newer launcher exists, `SelfUpdate.ReplaceWith` runs:
   - Download to `%TEMP%`, verify the SHA-256 and extract to `<exe>.stage`.
   - Find the binary under the name `WaWLauncher` or `WarriorsAndWizards`.
   - Rename the running exe to `.old` and move the new one into place.
   - Restart.
   - The self-update archives deliberately contain the launcher under the OLD name, so launcher 1.0.0 accepts them.
3. Pick the game archive for the platform with `Platforms.Pick`: Windows, Linux, or Mac by chip (arm64 or x64).
4. The result is NotInstalled, UpdateAvailable or Ready. PLAY becomes INSTALL or UPDATE.
5. `InstallAsync` refuses while the game runs (process name plus path check), then:
   - Downloads with progress and a 30-minute HTTP timeout.
   - Verifies the SHA-256.
   - Extracts (zip or tar.gz) to `.waw-staging`.
   - Swaps `Game/` with renames and rolls back if the swap fails.
   - Saves `gameVersion` and sets the exec bit.
6. "Repair" means `ForgetInstalledVersion`.

**Login tokens.**
- `AccountApi` posts form data to `https://play.<domain>/api/account/remember` (password in, `waw-token:` token out), then uses `account/verify`, `account/forget` and `account/register`. The answers are XML.
- The token is saved in `%LocalAppData%\WaWLauncher\login.json`. On Windows it is DPAPI-encrypted (CurrentUser) with a `dpapi:` prefix; on Linux and Mac it is plain text with mode 0600.
- Server side: tokens are stored as SHA-256 in the `login_tokens` table. They are accepted wherever a password is (`DbClient.VerifyAccount`) and expire if unused for a month, per the comment in `Account.cs`.

**Launching the client.**
- `Launch(user, token)` refuses without both values.
- It starts `Game/WarriorsAndWizards(.exe)` with environment variables `WAW_LAUNCH_USER` and `WAW_LAUNCH_TOKEN`. The client reads them once at start (`WaWClient/AppEngine/LaunchLogin.cs`).
- Published (`TARGET_VPS`) clients use launcher-only login (`ClientPlatform.LauncherOnlyLogin`).

**Platforms.** win-x64, linux-x64, osx-arm64 and osx-x64. Each is published self-contained, single-file and trimmed.
- Windows: `.lnk` shortcuts through a hand-rolled ShellLink vtable, and the HKCU `...\Uninstall\WarriorsAndWizards` entry.
- Linux: `.desktop` entries.
- Mac: an app bundle from `Tools/Mac`, without shortcuts. It is signed ad hoc by the SDK, has no Developer ID and needs Gatekeeper's "Open Anyway". Untested on a real Mac.

**Links.** Patch notes, website and download page all point to `https://warriorsandwizards.com/...`.

**Dependencies.**
- NuGet: Avalonia 12.1.3 (Desktop, Fonts.Inter, Themes.Fluent), System.Security.Cryptography.ProtectedData 10.0.0.
- Assets: `art.png`, `icon.png`, the `NotJamSignature21.ttf` font; the icon `WaW-Client/WaWClient/waw.ico`.
- No dependency on the game projects, on purpose.

**Decision for the migration: KEEP and adapt.**
- The launcher's contract is engine-agnostic: a manifest, archives and an executable with two environment variables. A Unity player build is just another archive.
- Required changes:
  - Accept the Unity output layout.
  - Make the Unity client read `WAW_LAUNCH_USER` / `WAW_LAUNCH_TOKEN`.
  - Keep `/download/manifest.json` hosting.
  - Keep `/account/remember|verify|forget|register` on the Account/API service. JSON responses are possible, but the launcher parses XML today, so keep XML or ship launcher 3.1 first.
- Recommended hardening (later): sign the manifest; keep the account API on HTTPS only.

## 3. Installer, in depth (`Installer/`)

- **Files:** `InstallerWindow.cs` (137 lines) and `Setup.cs` (97 lines). It links the launcher's `UiKit`, `EmberField`, `Install`, `Shortcuts`, `Updater`, `Manifest` and `AppsEntry`, plus assets with identical resource names. Shared files must stay free of launcher-only code.
- **Flow** (`Setup.RunAsync`):
  1. Fetch the manifest; the download base is the same as the launcher's, with a DEBUG override.
  2. Take `launcher.<platform>` and download it with progress, then check the SHA-256.
  3. Extract and run `PlaceLauncher`: copy `WaWLauncher` (or `WarriorsAndWizards`) as `WaWLauncher(.exe)` and save the shortcut choices in `.wawlauncher.json`. An existing install keeps its game.
  4. `Shortcuts.Apply`; on Windows also `AppsEntry.Write`.
  5. Optionally open the launcher.
  - `SuggestedFolder()` reuses the folder of an earlier install found in the registry.
- **Published:** `WaW-Installer.exe` (win-x64) and `WaW-Installer-linux.tar.gz`, built and uploaded by `deploy -Launcher`. There is no Mac installer.
- **Tests:** `Launcher.Tests/InstallerTests.cs` (`extern alias installer`).
- **Decision:** **KEEP as-is.** It does not depend on the game engine at all, only on the manifest's `launcher` entry.

## 4. Portal, in depth

**Pages** (`Portal/site/*.html`, one `js/portal.js`, `css/portal.css`):

| Page | Data |
| --- | --- |
| index | Search, online count, top-10 leaderboards |
| player | Profile by name |
| guild | Guild page |
| leaderboards | `kind` = `fame`, `chars`, `level` or `guilds` |
| items, classes | Wiki, from static JSON |
| graveyard | Static placeholder |
| releases | Redirect to `warriorsandwizards.com/patch-notes/` |

Pretty URLs (`/player/Name`, `/guild/Name`, `/top/fame`, `/wiki/item/...`) are nginx `try_files` rewrites in `setup_portal.sh`.

**Data generation** (`Tools/Portal/build_portal_data.py`):
- Reads `WaW-Client/WaWClient/Content/Xmls/*.xml`, `Content/Game.atlas`, `Content/Sheets/*.png` and the `BuildVersion` in `Core/Settings.cs`.
- Writes `data/items.json` (item type, name, tier, slot, category, description, stat bonuses, damage, icon), `data/classes.json`, `data/build.json`, and icons `icons/items/<hex>.png` and `icons/classes/<hex>.png` scaled x3.
- Its category rules duplicate `Shared/Common.Protocol/ItemCategories.cs`.

**The public API it uses.** These are read-only AccountServer endpoints in `AccountServer/Systems/Public/PublicHandlers.cs`, backed by `Common/Database/PublicDb.cs`:

| Endpoint | Used by |
| --- | --- |
| `/public/online` | Site and in-client Portal |
| `/public/search?q=` | Site and in-client Portal |
| `/public/player?name=` | Site and in-client Portal |
| `/public/guild?name=` | Site and in-client Portal |
| `/public/leaderboard?kind=` | Site and in-client Portal |
| `/public/releases` | The external website's patch-notes page (`CLAUDE.md`) |

- Requests may be GET or a POST form.
- Answers are cached 60 s on the server, with a limit of 240 requests per visitor per minute (`VPS_SETUP.md`; the code was not re-verified).
- The answers carry `Access-Control-Allow-Origin: *` (`AccountServer/Program.cs` line 233).
- nginx exposes them as `https://portal.<domain>/api/public/...`, adds `Cache-Control: public, max-age=30` and returns 404 for the rest of `/api/`.

**In-client Portal.**
- `WaWClient/Screens/Components/Portal/PortalView{,.Pages,.Wiki}.cs` show it, with `Data/PortalData.cs` and `AppEngine/PortalRequests.cs`.
- It calls the same `/public/*` endpoints directly on `AppEngineUrl` (plain HTTP 8080), from the title screen's PORTAL row.
- The Patch Notes and Releases parts were removed 2026-10-01.

**Decisions.**
- Portal site: **KEEP as-is.** The Account/API service must provide the same `/public/*` JSON.
- `build_portal_data.py`: **KEEP**, adjusting its paths when content moves.
- In-client Portal: **Rewrite later**, low priority. For the first Unity milestone, open the website in the browser instead.

## 5. Tools - verdicts for the new architecture

| Tool | Purpose | Inputs | Outputs | Verdict | Reason |
| --- | --- | --- | --- | --- | --- |
| `Tools/Editor/editor.html` + `editor.js` (+ `open_editor.cmd`) | Browser map editor (.jm), zones, item / object maker | `palette.js`, `.jm`, world JSON | `.jm`, JSON zones, XML snippets | **KEEP** | The only way the hand-made Nexus and Realm are maintained. It stays valid as long as `.jm` and XML remain the content formats. Replace with Unity editor tooling only if the formats change. |
| `Tools/Editor/build_palette.py` | Builds `palette.js` for the editor | XMLs, `Game.atlas`, `MapData.cs`, world configs | `palette.js` (112 KB) | **KEEP** | Needed by the editor. Its `MapData.cs` region parsing must be re-pointed when the C++ server owns regions. |
| `Tools/Editor/preview_map.py` | Offline PNG render of a map | `.jm`, XMLs, atlas, sheets | PNG | **KEEP** | Cheap layout check with no engine. |
| `Tools/Editor/tests/run_tests.py` | Headless browser tests of the editor against real maps (round-trip identical tiles) | maps, Chrome or Edge | PASS / FAIL | **KEEP** | Guards map integrity; useful in CI. |
| `Tools/Portal/build_portal_data.py` | Portal JSON and icons | XMLs, atlas, sheets, version | `Portal/site/data`, `icons` | **KEEP** | The Portal is kept, and the tool is pure data. |
| `Tools/Mac/build_mac_app.py` | Mac .app zip for the launcher | Launcher osx publishes | `WaW-Mac.zip` | **KEEP** (tied to the Launcher) | Wraps the launcher, not the game. Retire it if the launcher is dropped on Mac. |
| `Tools/Admin/reset_characters.sql` + `_local` / `_vps` `.ps1` / `.cmd` | Wipe every character, keep accounts | DB | DB plus backup | **REWRITE** | Hard-coded to the `accounts.data` JSONB layout, the PG 17 path and the VPS host address. Belongs in the API service as an admin operation or migration. |
| `Tools/E2E/e2e_roles.py`, `e2e_skins.py` | Live protocol tests (roles, tutorial, skins) | local servers | PASS / FAIL | **KEEP** | A behavioural acceptance test for the C++ GameServer and the API, as long as the wire protocol is kept. Keep the protocol helpers. |
| `WebClient/tools/build_web.py`, `patch_sources.py`, `patch_rules_more.py`, `port_shaders.py`, `shadercheck.py`, `enumgen` | Browser build of the C# client | desktop sources | WASM site | **RETIRE** | Unity WebGL replaces the browser build. |
| `WebClient/tools/webtest.py` + `acts_*.json` | Headless browser smoke tests | site URL | console, screenshot | **REPLACE** | Use a Unity WebGL or Playwright smoke test. Keep the action flows as reference. |
| `WebClient/tools/serve.py` | Local static server and `/api` proxy | dist | HTTP | **RETIRE** | Dev convenience for the WASM client. |
| `WebClient/tools/ws_bridge.py`, `WebClient/vps/ws_bridge.py` | WebSocket-to-TCP bridge | WS, TCP | relay | **REPLACE** | Either native WebSocket support in the C++ GameServer or a maintained bridge. Needed only while a WebGL client exists. |
| `WebClient/tools/errs.py`, `rebuild.sh` | Build-log helper, rebuild loop (stale scratchpad path) | logs | text | **RETIRE** | Personal dev loop. |
| `WebClient/vps/patch_cache.py` | One-off nginx cache fix | nginx conf | conf | **RETIRE** | Already applied; the fix is in `setup_web.sh`. |
| `WebClient/vps/setup_web.sh`, `Portal/vps/setup_portal.sh`, `Forums/vps/setup_forums.sh` | Imperative VPS provisioning | domain | nginx, systemd, TLS | **REPLACE** | Use versioned config (nginx or Caddy files, systemd units or compose file) applied by a script; see section 6. |
| `deploy.ps1` / `deploy.cmd` | Build, package, upload, restart, operate | repo | VPS state | **REPLACE** | Single-PC, root-SSH, imperative. See the recommendation in section 6. |
| `promote.ps1` / `promote.cmd` | Testing to Game folder mirror | folders | folder plus zip | **RETIRE** | Use git branches and tags. Also currently broken; see Discrepancies. |
| `WaW-Client/WaW.ContentBuilder` (build tool) | Atlas, font and copy pipeline | Content | built Content | **RETIRE** | Unity's importer replaces it. |
| `.config/dotnet-tools.json` (mgcb) | MonoGame content tools | - | - | **RETIRE** | Restored at every client build; no use found (UNVERIFIED). |

## 6. Deployment

### 6.1 `deploy.ps1` flags

Every flag is a switch unless a type is shown. With **no flags**, `-Server -Client` is assumed.

| Flag | What it does |
| --- | --- |
| `-Server` | `dotnet build` the server solution. Copy `WaW-Server/bin/debug/net10.0` to `dist/server-stage`. Write `-VpsHost` into `<Address>` of `appEngineConfig.xml` and `gameServerConfig.xml` (copy only). **Remove** the machine-owned files: `postgresConfig.xml`, `redisConfig.xml`, `rpcClientConfig.xml`, `rpcServerConfig.xml`, `rpc-server.cer`, `rpc-server.pfx`. Pack `dist/WaW-Server.tgz` and scp it to `/tmp`. Over SSH: seed `/opt/WaW/settings` from the old files if missing, `systemctl stop alloy-game alloy-account`, untar into `/opt/alloy-server`, start, `sleep 12`, `is-active`, show the last 3 log lines. Note: it builds and ships the **Debug** configuration. |
| `-Client` | Publish `WaWClient.csproj` Release win-x64 self-contained single-file with `-p:DeployTarget=vps`. `Assert-BinaryHasAddress`: the DLL must contain the VPS address and no `127.0.0.1`. Copy the built `Content/` and `soft_oal.dll` into `runtimes/win-x64/native`. Zip to `dist/WarriorsAndWizards-Client.zip`, scp to `/var/www/warriors/download/`. `Update-Manifest`: version plus the windows entry (size and sha256 computed on the VPS). |
| `-Linux` | Same for linux-x64, with `libopenal.so` and a `run.sh`. tar.gz, upload, re-tar on the VPS to set exec bits, manifest `linux`. |
| `-Mac` | Same for osx-arm64 and osx-x64, with `libopenal.dylib`. Two tar.gz files, exec-bit fix, manifest `macArm64` / `macX64`. |
| `-Launcher` | Publish Launcher and Installer for win-x64 and linux-x64. Self-update archives `WarriorsAndWizards-Launcher-windows.zip` / `-linux.tar.gz` (binary renamed to the old name). `WaW-Installer.exe` and `WaW-Installer-linux.tar.gz`. Mac launchers for osx-arm64 and osx-x64 as `WarriorsAndWizards-Launcher-mac-*.tar.gz`, plus `WaW-Mac.zip` from `Tools/Mac/build_mac_app.py`. Upload everything, fix exec bits, manifest `launcher` (version from `Launcher.csproj`). |
| `-Manifest` | Repair: list `/var/www/warriors/download` and rebuild `manifest.json` from the archives present, with no upload. Not allowed with `-DryRun`. |
| `-NoUpload` | Build and package only. The VPS is untouched. |
| `-SkipWebBuild` | With `-Web`: upload the last built site without rebuilding. |
| `-Web` | `python WebClient/tools/build_web.py`, with `--aot` if `%USERPROFILE%\.dotnet-wasm\dotnet.exe` exists. tar to `dist/ww-site.tgz`. Upload and replace `/var/www/warriors/*` **except `download/`**. |
| `-SetupWeb` | scp `WebClient/vps` to `/root/ww-vps` and run `setup_web.sh <WebDomain>`. |
| `-SetupForums` | Requires `-ForumAdminEmail`. Prompts for the admin password (at least 8 characters, no spaces or quotes). scp `Forums/vps`, run `setup_forums.sh` (the password is passed on the ssh command line). |
| `-SetupPortal` | scp `Portal/vps` and run `setup_portal.sh <PortalDomain> <VpsHost>`. |
| `-Portal` | `python Tools/Portal/build_portal_data.py`, tar `Portal/site` to `dist/ww-portal.tgz`, upload, replace `/var/www/portal/*`. |
| `-PortalDomain <string>` | Default `portal.warriorsandwizards.com` |
| `-ForumDomain <string>` | Default `forums.warriorsandwizards.com` |
| `-ForumAdminUser <string>` | Default `admin` |
| `-ForumAdminEmail <string>` | Required for `-SetupForums` |
| `-SetVersion <x.y.z>` | Write the version into `WaW-Client/WaWClient/Core/Settings.cs` (`BuildVersion`) and `gameServerConfig.xml` (`<Version>`), keeping the BOM. Exits unless combined with deploy flags. Every non-ops run also **refuses** to continue if the two versions differ. |
| `-Status` | Over SSH: `systemctl is-active` for `alloy-account`, `alloy-game`, `postgresql`, `redis-server`, `nginx`, `ww-bridge`; uptime, memory, disk; connection counts on 2050 and 8080; the last game log lines. |
| `-Logs` | `journalctl -u alloy-game -u alloy-account`, last `-LogLines` lines (default 80) |
| `-Follow` | With `-Logs`: live tail |
| `-LogLines <int>` | Default 80 |
| `-Since <string>` | journalctl time window |
| `-Filter <string>` | grep pattern; commas mean OR |
| `-Load` | Filter preset `STATS,PLAUSIBILITY,FLOOD,LAGGED,late=,Error` |
| `-Bridge` | Adds the `ww-bridge` unit, kernel flood / conntrack lines, `ss -s`, the top nginx client addresses and the nginx error log |
| `-Restart` | `systemctl restart alloy-account alloy-game`, then a check |
| `-Backup` | `pg_dump alloy` piped to gzip on the VPS, scp to `-BackupDir`, `Assert-GoodBackup` (gzip readable, header and "dump complete" trailer present) |
| `-BackupDir <string>` | Default `Documents\WW_Backups` |
| `-Shortcuts` | Creates `/opt/WaW` with symlinks (server, server-settings, maps, patch-notes, website, nginx conf, browser-bridge, backups, services), `settings/`, a README, three launcher scripts and `.desktop` files, and copies them to every user's Desktop |
| `-DryRun` | Forces `-NoUpload`. The ops commands only print. Refused with `-SetupWeb`, `-SetupForums`, `-SetupPortal` and `-Manifest`. |
| `-WebDomain <string>` | Default `play.warriorsandwizards.com` |
| `-VpsHost <string>` | Default: the VPS host address (literal in the script) |
| `-VpsUser <string>` | Default `root` |

Wrapper: `deploy.cmd` runs `powershell -NoProfile -ExecutionPolicy Bypass -File deploy.ps1 %*`. `promote.cmd` does the same for promote.

### 6.2 Current VPS layout (from `deploy.ps1`, `VPS_SETUP.md` and the setup scripts)

| Path or unit | Content |
| --- | --- |
| `/opt/alloy-server/` | AccountServer.dll, GameServer.dll, Common.dll and `Resources/` (configs, XML, maps, news). Replaced by every `-Server`. The VPS-only secret configs live in `Resources/Config/Data`. |
| `/opt/WaW/settings/` | Owner settings (never overwritten) |
| `/opt/WaW/` | Symlink hub, README, launchers (`-Shortcuts`) |
| `/var/www/warriors/` | Web client (WASM) site; `download/` holds the client and launcher archives, installers, `WaW-Mac.zip` and `manifest.json` |
| `/var/www/portal/` | Portal static site |
| `/opt/ww-bridge/` | venv and `ws_bridge.py` |
| `/opt/nodebb/` | NodeBB (`config.json` holds the DB password) |
| `/var/backups/waw/` | Backup folder (mode 700) |
| systemd `alloy-account` | `dotnet /opt/alloy-server/AccountServer.dll`, Restart=on-failure |
| systemd `alloy-game` | `dotnet .../GameServer.dll`, `ExecStartPre=sleep 6`, after `alloy-account` |
| systemd `ww-bridge` | `ws_bridge.py 2051 127.0.0.1 2050`, Restart=always |
| systemd `ww-forums` | NodeBB on 127.0.0.1:4567 |
| `postgresql`, `redis-server`, `nginx` | apt packages; Postgres and Redis on localhost |
| nginx `conf.d/warriors.conf` | `play.<domain>`: `/` static, `/_framework/` caching rules, `/download/`, `/api/` to `<ip>:8080` (`Host: <ip>:8080` rewrite, `X-Forwarded-For`), `/game` WebSocket to 2051 |
| nginx `sites-available/portal` | `portal.<domain>`: static, pretty URLs, `/api/public/` to `<ip>:8080/public/` |
| nginx `conf.d/forums.conf` | `forums.<domain>` to 4567 |
| ufw | 22, 8080, 2050 (VPS_SETUP), 80 and 443 (setup scripts). 8081, 5432, 6379 and 4567 must stay closed. |
| TLS | certbot `--nginx`, auto-renewed, per subdomain |
| Website | `warriorsandwizards.com` on Cloudflare Pages (separate repo). `_redirects` rule for `/downloads/...` to the VPS. |

Notes:
- The AccountServer binds to the VPS's public address on 8080. It rejects a request whose `Host` header is not its listen address, which is why nginx rewrites `Host`.
- The desktop client talks to 8080 over **plain HTTP from the internet**, so passwords and tokens are not encrypted on that path.
- The launcher and dashboard use HTTPS through nginx.

### 6.3 RECOMMENDATION - modern deployment without unnecessary early infrastructure

The goal is to replace the hand-run, root-SSH, Debug-build deploy with something reproducible, while staying on **one VPS** at first.

1. **Source of truth: git plus CI.**
   - Add one monorepo, or a few repos: `unity-client`, `gameserver` (C++/CMake), `account-api` (C#), `content`, `portal`, `launcher`.
   - CI (for example GitHub Actions) builds release artifacts on every tag:
     - Unity player builds (Win / Linux / Mac) as zip or tar.gz.
     - The C++ GameServer as a Linux x64 binary, built in a pinned toolchain container.
     - The API as a `dotnet publish` Release linux-x64 output.
     - The Portal tarball.
     - The launcher and installer.
   - CI runs the unit tests and the E2E protocol tests against a CI-started server stack. This replaces `promote.ps1`.

2. **One VPS, systemd, no Kubernetes.** Keep the current model, with these differences:
   - Run each service as its **own non-root user**: `waw-gameserver.service` (C++ binary) and `waw-api.service` (ASP.NET Core Kestrel on 127.0.0.1:8080).
   - Put Postgres and Redis on localhost, as today.
   - Keep the unit files and proxy config **in the repo** and install them with one idempotent script, or a minimal Ansible playbook if a second host is ever needed.
   - Docker Compose is an acceptable alternative for Postgres, Redis and the API. Do **not** introduce Kubernetes, a message bus or a separate cache cluster.

3. **Reverse proxy:**
   - One nginx (or Caddy, for automatic TLS) in front of everything, using the same hostnames: `play.` (downloads, `/api/`, optional WebGL and `/game`), `portal.` (static and `/api/public/`) and `forums.` (optional).
   - **Close 8080 to the internet**: the Unity client should call the API over HTTPS through the proxy, which fixes the plain-HTTP password path.
   - Keep the GameServer TCP port (2050) open directly. Add native WebSocket to the C++ server only if a WebGL build ships; until then drop `ww-bridge`.

4. **GameServer to API communication:**
   - Replace the StreamJsonRpc-over-TLS hub with something the C++ server can speak easily, with a shared secret or mTLS on localhost: plain HTTP/JSON to internal API endpoints (simplest) or gRPC.
   - Keep the GameServer without direct DB access, as today. That is a good boundary.
   - Redis stays only for account locks and sessions.

5. **Releases:**
   - Upload artifacts to the VPS (rsync, or `scp` by a deploy user) into versioned folders such as `/opt/waw/releases/<ver>/`, and switch a `current` symlink. Restart with systemctl. Rollback means pointing the symlink back.
   - Generate `manifest.json` **in CI** from the artifacts, ideally signed, and publish it last, so launchers never see a half-uploaded release.
   - Keep the "version lock" rule (client = server version) but make it a protocol version, not the build version, so cosmetic client patches do not lock everyone out.

6. **Operations:**
   - journald and the existing `[STATS]` lines are enough at first. Add a nightly `pg_dump` systemd timer with off-box copy; this replaces `-Backup`.
   - Keep owner settings in the DB or in `/etc/waw/` and never in the release folder; this keeps today's good `/opt/WaW/settings` idea.
   - Secrets go in environment files (`/etc/waw/*.env`, mode 600) or systemd credentials. **Rotate** the RPC secret and certificate password that are committed in REF's config files.

7. **Portal and forums:**
   - The Portal stays static: it is deployed as a CI artifact, and its data is generated in CI by `build_portal_data.py`.
   - Forums: keep NodeBB on the same box only if RAM allows (NodeBB needs about 1 GB to build). Otherwise use a hosted forum or Discord.

## 7. Tests inventory

### 7.1 xUnit projects

Method counts come from `grep` for `[Fact` and `[Theory` attribute lines. `[InlineData]` rows multiply the Theory cases.

| Project | Path | Files | [Fact] | [Theory] | [InlineData] rows | In solution | Run by promote |
| --- | --- | --- | --- | --- | --- | --- | --- |
| WaWClient.Tests | `WaW-Client/Tests/WaWClient.Tests` | 35 | 175 | 38 | 197 | Client | yes |
| Common.Protocol.Tests | `Shared/Tests/Common.Protocol.Tests` | 6 | 32 | 7 | 56 | Client + Server | yes (twice) |
| Common.Tests | `WaW-Server/Tests/Common.Tests` | 25 | 212 | 27 | 157 | Server | yes |
| GameServer.Tests | `WaW-Server/Tests/GameServer.Tests` | 43 | 225 | 18 | 84 | Server | yes |
| Launcher.Tests | `Launcher/Tests/Launcher.Tests` | 2 | 22 | 2 | 11 | none | **no** |
| DeveloperDashboard.Tests | `DeveloperDashboard/Tests/DeveloperDashboard.Tests` | 1 | 7 | 3 | 9 | none | **no** |
| **Total** | | 112 | **673** | **95** | 514 | | |

All six use xunit 2.9.3, xunit.runner.visualstudio 3.1.4, Microsoft.NET.Test.Sdk 17.14.1 and coverlet.collector 6.0.4. `TestResults/` folders are present in four of them, so the tests have been run.

Other test harnesses:
- `Tools/Editor/tests/run_tests.py`: headless browser editor tests.
- `WebClient/tools/webtest.py` and `shadercheck.py`: browser smoke and shader checks.

### 7.2 E2E Python tests (`Tools/E2E`)

These are behavioural references for end-to-end testing. They run against **local** servers only, using raw sockets to 127.0.0.1:2050 and `urllib` to :8080.

The protocol facts they encode:
- Frame = `int32 LE total length (including the 5-byte header)`, then `byte packetId`, then the body.
- Strings = `uint16 LE length` followed by UTF-8.
- `Hello` = `version, int gameId, username, password, int 0`.
- The version is read from `Core/Settings.cs`.
- World ids: Nexus -1, Tutorial -7.
- Packet ids used: Failure 0, CreateSuccess 1, Create 2, PlayerText 5, Text 6, Update 9, NewTick 11, Hello 15, Reconnect 19, MapInfo 20, Load 21, Escape 41.

`e2e_roles.py` (also provides the helpers `Conn`, `api`, `enter`, `create_body`, `char_list`):
1. Register a throwaway `Rt*` account (`/account/register`).
2. `Create` without a role, or with a fake role, is refused (`Failure` mentioning the role); no character is created.
3. A new account's first character created as a Magica Wizard is routed to the **Tutorial**, not the Nexus it asked for.
4. `Escape` in the Tutorial leads to the Nexus.
5. `/char/list` shows Role=magica and RoleRequired=false. `/char/chooseRole` cannot change a chosen role.
6. Reconnecting later loads the Nexus, never the Tutorial.

`e2e_skins.py` (14 checks; local new accounts start with 100,000 gold and fame through `newAccountsConfig.xml`):
1. A new account owns no skins.
2. Buying Dark Wizard with gold works; buying it again is refused ("already own").
3. An **Earned** skin cannot be bought.
4. Buying Sage with fame works; a **Free** skin (Healer) is not sold; an unknown skin is refused.
5. The character list names the owned skins.
6. A Wizard cannot be made in an unowned skin (Druid) or in a Warrior skin (Paladin).
7. Creation in owned skins works (Dark Wizard, Paladin), and the characters wear them.
8. A fresh account can create the free Healer Wizard and Barbarian Warrior; nothing is stored in OwnedSkins.

Flows covered: registration, game-server Hello and Load, character Create with class, skin and role, world routing (Tutorial gating), Escape, the char list over HTTP, the skin shop over HTTP, and the HTTP/TCP interplay.

**Not covered:** combat, loot, movement or anti-cheat, the launcher token sign-in, `/public/*`, `/dev/*`, daily rewards and inbox.

`WebClient/tools/acts_*.json` cover browser flows: login, register, register then play, sign in then play, characters, new character, enter, play, persist.

## 8. Discrepancies (the docs say X; the files show Y)

1. **README "What is in this repository":** README lists `WaWClient/`; the folder is `WaW-Client/` (`WaWClient/` is a subproject). It describes Tools as "map editor, art-sheet and map generators"; the art-sheet and map generators were deleted on 2026-10-02 (`CLAUDE.md`). The only tools left are Editor, Mac, Portal, Admin and E2E. README does not mention Installer, DeveloperDashboard, Forums, Archive or ExtraAssets.
2. **README / VPS_SETUP vs deploy.ps1 (Mac):** `VPS_SETUP.md` describes the release order as `-Client`, `-Linux`, `-Launcher` and does not mention `-Mac`. `deploy.ps1` has `-Mac` and builds Mac launchers and `WaW-Mac.zip` under `-Launcher`.
3. **CLAUDE.md vs promote.ps1:** CLAUDE.md says the Game folder "stopped being a git repo on 2026-09-28" and is "a plain folder". `promote.ps1` **throws unless the target has a `.git` folder**, so `promote -Apply` would refuse as written. Also, `promote` runs only the two solutions' tests: Launcher.Tests and DeveloperDashboard.Tests are never run by it.
4. **CLAUDE.md tool paths:** CLAUDE.md mentions `Tools/ForestContent/preview_map.py`, `Tools/Sheets/*`, `Tools/Voxel/*`, `Tools/Models/*`, `Tools/BookUi/*`, `Tools/_retired`, `Tools/Music/make_music_config.py` and `Tools/Shore/*`. None exist. `preview_map.py` is at `Tools/Editor/preview_map.py`. CLAUDE.md itself marks these as "HISTORY" in its 2026-10-02 note, but its body still gives them as instructions.
5. **CLAUDE.md folder locations:** CLAUDE.md gives Testing as `C:\Users\cbart\Desktop\Repos\Warriors-and-Wizards-Testing`. REF is a copy under `Desktop\Runity\Reference - ...`. `promote.ps1`'s default `-Target` and `rebuild.sh`'s scratchpad path are hard-coded to the old locations.
6. **EngineeringAudit 1.1 / 1.6 / 1.7 / 3.8 / 3.10 (dated 2026-09-21) are stale:**
   - Names: `AlloyClient` and `alloy-server` instead of `WaW-Client` and `WaW-Server`.
   - Shared: "2 files"; it is now 10.
   - Database: 8 tables listed; there are 13 (`login_tokens`, `anticheat_flags`, `gift_items`, `gift_chests` and `patch_notes` were added).
   - Sizes: `dist/` "300 MB" and `WebClient/dist/` "765 MB"; now 1,911 MB and 1,274 MB. `Content/bin` "46 MB"; now 94 MB.
   - Tests: "349 facts, four test projects"; now 673 Fact plus 95 Theory methods in six projects.
   - Dependencies: F49 says GameServer references Google.Protobuf and Grpc; they were removed (`GameServer.csproj` comment). F49 says `RSA.cs` uses BouncyCastle; no `Org.BouncyCastle` usage remains in client or web sources, yet the package is still referenced by `WaWClient.csproj` and `web.csproj`.
   - F55 cites `Tools/Sheets/README.md`, which no longer exists.
7. **AccountServer crossdomain path:** `AccountServer.csproj` copies `Handlers\Crossdomain\crossdomain.xml`, and `Crossdomain.cs` reads `Handlers/Crossdomain/crossdomain.xml`. The file is at `AccountServer/Systems/Crossdomain/crossdomain.xml`, so the copy rule matches nothing. `/crossdomain.xml` probably fails at runtime (UNVERIFIED). It is a legacy Flash artifact.
8. **ws_bridge docstring:** `WebClient/vps/ws_bridge.py` (identical to `tools/ws_bridge.py`) calls itself a "local stand-in for the VPS's websockify". `setup_web.sh` installs **this same script** as the production `ww-bridge` service; no websockify exists. Its default target is the VPS host address, but production passes `127.0.0.1 2050`.
9. **Browser players' IP:** CLAUDE.md says every browser player's real address arrives through nginx `X-Forwarded-For`. That holds for HTTP `/api/` only. On the game socket the bridge connects from 127.0.0.1 and forwards no client address. `gameServerConfig.xml` confirms that all browser players share the loopback address, which is exempt from the per-IP cap. So per-IP limits and bans do not apply to WebGL/WASM players on the game port.
10. **serve.py vs AccountServer:** `serve.py` says "the account server sends no CORS headers". `AccountServer/Program.cs` sends `Access-Control-Allow-Origin: *` on `/public/*` (added later).
11. **patch_sources.py** says "build.ps1 does" run it. No `build.ps1` exists; `build_web.py` runs it.
12. **WebClient/README.md** says `web.csproj` compiles sources "from ../../WaWClient"; the csproj uses `..\..\WaW-Client\`.
13. **.gitignore vs presence:** `.gitignore` ignores `CLAUDE.md`, `VPS_SETUP.md`, `postgresConfig.xml`, `rpc*Config.xml` and `rpc-server.*`. All the config files are present in REF, and the RPC configs contain live-looking secrets. Treat REF as sensitive.
14. **README "Run locally" path:** the server exe path `WaW-Server/bin/debug/net10.0/AccountServer.exe` is consistent with the csproj `OutputPath` `..\bin\debug` (plus the TFM). `deploy -Server` ships this **Debug** build to production; VPS_SETUP step 5 does the same. No document calls this out.
15. **VPS_SETUP firewall:** VPS_SETUP opens 22, 8080 and 2050. The setup scripts additionally open 80 and 443. VPS_SETUP section 10 mentions nginx but not the 80/443 rules (minor).
16. **Content.xml:** it declares an `Fbx` builder for `Objects/`; there is no `Content/Objects` folder (the 3D models were removed 2026-10-01).
17. **realmConfig.xml** still lists upstream realm events (Ghost ship, Hermit God, Kage Kami) whose behaviours are archived in `Archive/ServerBehaviors`. Whether they are dead config is UNVERIFIED.
18. **Launcher patch-notes link:** `Portal/site/releases.html` is a redirect kept "for launchers up to 3.0.3", but `LauncherWindow.cs` in 3.0.3 already points at `warriorsandwizards.com/patch-notes/`. The redirect only matters for older launchers. Minor.
19. **dist leftovers:** the `deploy.ps1` comment says `WarriorsAndWizards-Setup.exe`, `WaWLauncher.exe` and `WaWLauncher-linux.tar.gz` "are no longer uploaded". They and `alloy-server.tgz` are still in `dist/`. They are stale artifacts.
