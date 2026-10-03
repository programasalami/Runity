# Unity Migration Plan - Client

Companion to `ClientArchitecture.md` (same folder), which holds the reference behaviour with file/line citations. Paths starting with
`WaW-Client/`, `WebClient/`, `Shared/` are relative to the reference source; `Assets/...` paths are in the Unity project
`C:\Users\cbart\Desktop\Runity\WaW`.

Target: Unity **6000.6.4f1**, URP **17.6.0**, Input System **1.20.0** (active input handler = Input System only,
`ProjectSettings.asset` `activeInputHandler: 1`), uGUI **2.6.0** (includes TextMeshPro), UI Toolkit (module `uielements`), Test Framework
**1.8.0**, Timeline 6.6.0. Transitively present (`Packages/packages-lock.json`): Shader Graph, 2D Sprite (`com.unity.2d.sprite`),
Burst, Collections, Mathematics, Newtonsoft JSON. **Not installed**: Addressables, VFX Graph, 2D Tilemap Editor, any networking package.
The project is still the URP template (`Assets/Scenes/SampleScene.unity`, `Assets/Settings/PC_RPAsset.asset`, `Mobile_RPAsset.asset`).

Every subsystem below is **Implementation status: NOT STARTED**.

---

## 1. Mapping: old responsibility -> new Unity responsibility

| Old (reference) | New (Unity) |
|---|---|
| `WaW.Engine/GameWindow.cs` loop, `GameTime` ms, frame cap, timer resolution, GPU timer | Unity player loop (`Update`/`LateUpdate`), `Time.deltaTime` (keep game logic in ms or convert once at the boundary), `Application.targetFrameRate` + `QualitySettings.vSyncCount`, Unity Profiler / `FrameTimingManager` |
| `Main.Initialize/LoadContent`, `LoadPlan` | A bootstrap scene + `Bootstrap` MonoBehaviour running async load steps (Awaitable / coroutines); loading screen in UI |
| `Display/ScreenManager` + `Screen` subclasses | Scene/state machine: a persistent `AppRoot` with screen states (Title, Book, Creation, Portal, Loading, Game, Death); UI Toolkit documents per screen; one additive Game scene for the world |
| `OverlayManager`, `DialogManager`, `TooltipManager` | UI Toolkit layers (sort order) with a modal stack service |
| `WaW.UiLib` (Stage/Sprite tree, events, GTween, Signals, SpriteRender, MSDF text) | **UI Toolkit** (UXML/USS, `PanelSettings` scale-with-screen-size, reference 1280x720) for menus, HUD and windows; C# events instead of `Signal`; USS transitions or a small tween helper instead of GTween; TextMeshPro/UI Toolkit text with SDF fonts instead of the MSDF builder. uGUI only where UI Toolkit lacks something (UNVERIFIED need) |
| World-space UI (names, HP bars, speech bubbles, damage text) | Pooled world-space sprite/TMP objects or a screen-space UI Toolkit overlay positioned via `Camera.WorldToScreenPoint` |
| `Game.atlas` / `Ui.atlas` binary atlases, `ContentBuilder`, `ContentReader` | Imported PNG sheets sliced as Sprites (2D Sprite package present), packed by **Sprite Atlas v2**; lookup by art key through a catalog ScriptableObject. Addressables optional later (not installed) |
| Content XML parsed at runtime (`AssetParser`) | Definitions imported at edit time into ScriptableObject/`DefinitionCatalog` (or a compact JSON/binary blob) generated from the shared definition source; long term the C++ server owns definitions and may send a version hash (see section 6) |
| `WaW.Audio` OpenAL thread | `AudioSource` + `AudioMixer` (Master/Music/SFX groups exposed params for the three volume sliders), music crossfade with two sources |
| `ParticleEffects/*`, rain, splashes, fireflies, fire glow | **Shuriken `ParticleSystem`** (module installed; VFX Graph not installed and not WebGL-capable); pooled systems |
| GLSL shaders (Ground/Object/Model/Shadow/Particle/Fog/Bloom/Ui) | URP Shader Graph or hand-written URP HLSL: `WaW/Ground` (tile + blend masks + water), `WaW/Sprite` (billboard, grade), `WaW/Wall`; URP Volume **Bloom** instead of `Bloom.frag`; fog as a URP Full Screen Pass Renderer Feature; colour grade as global shader properties (or Volume Color Adjustments / custom LUT) |
| Shadows (silhouette + contact spot into a MIN-blended target) | Phase 1: contact-spot quads only (cheap, ordered under sprites); Phase 2: silhouette quads with a custom shader, or a dedicated renderer feature rendering a shade texture. Not URP shadow maps (sprites are billboards) |
| `Lights.cs` 12 nearest lights, `SightMap` | Global shader arrays set from C# each frame (keeps parity), or URP Forward+ additional lights (UNVERIFIED cost on low-end); sight map as a small `Texture2D` updated from C# |
| `DayNight.cs`, `Weather.cs` | Ported **verbatim as pure C#** (no UnityEngine) in Domain; Presentation reads them |
| `Camera.cs` (ortho, rotation, zoom, tilt 1.0) | One orthographic `UnityEngine.Camera` looking down (world X/Y on Unity X/Z), yaw = camera angle, size from zoom; Cinemachine not needed |
| `UserInput.cs` + `Settings` scancodes | Input System actions in `Assets/InputSystem_Actions.inputactions` (replace the template map with a `Gameplay` and `UI` map), rebinding UI, overrides saved with `SaveBindingOverridesAsJson` |
| `Core/Settings.cs` -> `settings.xml` | `SettingsModel` (plain C#) serialized to JSON in `Application.persistentDataPath`; `PlayerPrefs` only for trivial values. Never store the password; store a launcher-style token |
| `Networking/Client.cs` (TCP, SAEA, ConcurrentQueue) | `Net` assembly: plain C# `TcpTransport` on a background thread -> `ConcurrentQueue` -> drained on main thread; later `WebSocketTransport` for WebGL |
| `AppEngine/*` HTTP | `UnityWebRequest` (module installed) or `HttpClient` wrapped in an `AccountApi` service; async/await |
| `Game/Map.cs` static world | `WorldState` instance (Domain) owned by a `GameSession`; no statics |
| `Entity`/`Player` (logic + render mixed) | Split: `EntityModel` (Domain) + `EntityView` (Presentation MonoBehaviour, pooled) |
| `Projectile` + paths | `ProjectileModel` + `ProjectilePathMath` (Domain, deterministic) + pooled `ProjectileView` |
| `MinimapTexture` 4096x4096 | `Texture2D` sized to the map (`MapInfo` width x height), `SetPixels32` dirty rect, RawImage/UI Toolkit `Image` |
| Debug/perf (DebugStats, PerfSections, PacketLogger, DevPerfTest) | Unity Profiler markers (`ProfilerMarker`), a toggleable debug overlay, `Debug.Log` with a log-level filter; packet logger kept in Net |
| `Tests/WaWClient.Tests` (xUnit) | Unity Test Framework EditMode (NUnit) for Domain/Protocol/Net; PlayMode for views/UI smoke |

---

## 2. What NOT to port

- **Intel HD 4400 / GL driver workarounds**: buffer orphaning (`InstanceAttributeBuffer`), the 32-set UI buffer ring (`SpriteRender.cs:23`), "no instancing", `textureLod` vs derivative rules, flattened struct varyings, `GraphicsProfile` Compatible/Fast and `Settings.RendererMode`, the fixed texture-unit table, `GpuFrameTimer`, `ScreenPixels` Retina handling, Windows `timeBeginPeriod`, OpenGL 3.3 constraints, `StorageBuffer` data-texture emulation of SSBOs, 64 KB shadow UBO layout.
- **Engine plumbing**: `WaW.Engine` GL wrappers, `WaW.ShaderSourceGen`, `WaW.ContentBuilder`/`WaW.ContentReader` and the binary `.atlas` format, MSDF font builder, `StaticProps` CPU baking (use static batching / SRP Batcher / GPU instancing), `TileChunkMesh` graveyard thread hack, `FixedStepper` (use Unity's own `FixedUpdate` or a domain stepper only if needed).
- **UiLib**: Stage/Sprite tree, EventManager, GTween, Timer, Signals (weak refs), SpriteRender, `Overlay.FixedSize` maths - replaced by UI Toolkit.
- **Web patching**: everything in `WebClient/` (shims, `patched/`, `port_shaders.py`, `patch_sources.py`, `patch_rules_more.py`, `enumgen`, `shadercheck.py`, `build_web.py`, `serve.py`, `webtest.py`, `acts_*.json`, `ws_bridge.py`, `vps/*`).
- **Dead code**: crossed cards / flat stack / 3D model enum (`Render.Draw.cs:330, 369`, `Render.Baked.cs`, `ModelData.cs`), `ShoreCoverage`, non-interpolated remote path (`Entity.cs:204-220`), empty packet handlers unless the feature is wanted.
- **Developer conveniences tied to the old PC**: `WAW_NO_STATIC_BAKE`, `WAW_PERFTEST*` harness (rebuild later as a Unity test scene if needed).
- **Not to port as-is (security)**: Base64 password in `account.xml`, password on every HTTP call and in `Hello`.

---

## 3. WebClient decision

**Recommendation: retire the WebAssembly shim build entirely.** Do not port any of `WebClient/`.
- It exists only to run OpenTK/GL code in a browser; Unity removes that need.
- A **Unity WebGL build is a later option**. It needs: (a) a `WebSocketTransport` implementing the same `ITransport` as TCP (Unity WebGL has no `System.Net.Sockets`; a JS-plugin WebSocket or a maintained package - UNVERIFIED which), (b) the new C++ server to accept WebSocket connections directly or sit behind a WebSocket->TCP bridge (the current `/game` websockify pattern), (c) same-origin HTTP for the account API (CORS), (d) no background threads in the transport on WebGL (poll on main thread), (e) shaders kept WebGL2-compatible (avoid compute, keep Shader Graph targets that support WebGL).
- Design the `Net` assembly with an `ITransport` interface from day one so WebGL costs only a new transport class.

---

## 4. Proposed Unity project structure

```
Assets/
  _Project/
    Scripts/
      Protocol/        WaW.Protocol.asmdef      noEngineReferences: true   packet ids, readers/writers, DTOs (generated or hand-written to match the C++ server)
      Net/             WaW.Net.asmdef           noEngineReferences: true   ITransport, TcpTransport, (later) WebSocketTransport, framing, packet queues, AccountApi (HttpClient)
      Domain/          WaW.Domain.asmdef        noEngineReferences: true   WorldState, TileMap, EntityModel, PlayerController (movement/collision rules),
                                                                            ProjectileModel + paths, ConditionEffects, InventoryLayout use, DayNight, Weather,
                                                                            SettingsModel, definitions (ObjectDef/GroundDef/ItemDef) as plain C#
      Content/         WaW.Content.asmdef       UnityEngine                 DefinitionCatalog (ScriptableObject), ArtCatalog (key -> Sprite/AnimationSet), AudioCatalog
      Presentation/    WaW.Presentation.asmdef  UnityEngine, URP             GameSession MonoBehaviour, EntityViewRegistry, pooled views, TileChunkRenderer,
                                                                            CameraRig, InputBridge, Lighting/Weather/Particles, Minimap texture, Audio
      UI/              WaW.UI.asmdef            UnityEngine, UIElements      screens, HUD, windows, options, book, creation, portal, death
      App/             WaW.App.asmdef                                       bootstrap, screen state machine, service wiring
    Editor/            WaW.Editor.asmdef        Editor only                  definition importer (XML -> catalog), sprite sheet slicer, validators
    Tests/
      EditMode/        WaW.Tests.EditMode.asmdef  -> Protocol, Net, Domain, Content
      PlayMode/        WaW.Tests.PlayMode.asmdef  -> Presentation, UI, App
    Art/ Sheets/ UI/ Fonts/ Audio/ Shaders/ Materials/ Prefabs/ Scenes/ Settings/
```

Dependency direction: `Protocol <- Net`, `Protocol <- Domain` (only DTO mapping in a thin adapter if desired), `Domain <- Content`,
`Domain/Content/Net <- Presentation`, `Presentation <- UI` (UI reads Domain state, sends intents), `App` references all. Domain never
references UnityEngine, so its tests run in EditMode in milliseconds and can be cross-checked against the C++ server's rules.

---

## 5. Entity flow and view registry

```
Socket thread:  bytes -> frame -> Protocol packet (pooled) -> ConcurrentQueue
Main thread (GameSession.Update, start of frame):
  drain queue -> PacketRouter -> WorldState mutations
     Update.NewObjs   -> WorldState.AddEntity(EntityModel)      -> event EntityAdded(id)
     Update.Drops     -> WorldState.RemoveEntity(id)            -> event EntityRemoved(id)
     Update.Tiles     -> TileMap.Set(x,y,type)                  -> event TilesChanged(chunk)
     NewTick.Stats    -> EntityModel.ApplyStats + PushSnapshot  -> (no event; views poll in LateUpdate)
     EnemyShoot/...   -> ProjectileSystem.Spawn                 -> event ProjectileSpawned
  local input -> PlayerController.Step(dt) (prediction)
  ProjectileSystem.Step(dt) (paths, wall tests, hit tests)
Presentation (LateUpdate):
  EntityViewRegistry: id -> EntityView; on EntityAdded take a view from the pool chosen by ArtCatalog/definition
  (SpriteEntityView, WallView, ...), bind(model); on EntityRemoved return to pool
  each view reads model.RenderPosition (interpolated), facing, animation state, visibility (culling) and updates its transform/sprite
  TileChunkRenderer rebuilds dirty chunk meshes; MinimapService writes dirty pixels
```

Rules: views never mutate models; models never reference views; registry keyed by server object id; pools per view prefab type;
`WorldState.Clear()` on world switch returns every view to its pool (fixes the reference's partial-clear history, `Map.ClearWorldObjects`).

---

## 6. Asset replaceability

- Definitions reference art **by key** (today `<Texture><File>sheet</File><Index>n</Index>`, `<AnimatedTexture>`; `AssetParser.cs:96-147`). Keep that idea: an art key such as `oryxFantasy16World1:12` or a stable name.
- `ArtCatalog` ScriptableObject: key -> `Sprite` (static) or `SpriteAnimationSet` (FaceRight/FaceDown/FaceUp arrays, the 7-cell hero layout: idle, walk1, walk2, idle2, attack1, attack2 double width). A missing key resolves to THE placeholder sprite (reference behaviour: unknown objects draw the placeholder, `Update.cs:76-79`).
- Sheets imported as Sprite (Multiple) with grid slicing by an editor importer reading the same recipe data as `Content/Game.atlas` / `Ui.atlas` (name, cell w/h, Image vs Animated); packed with Sprite Atlas v2. Swapping art = replacing a PNG or re-pointing keys, no code change.
- Definitions: today the client folder `WaW-Client/WaWClient/Content/Xmls/*.xml` is the single source for client AND server (`WaW-Server/Common/Common.csproj:143-160` links them). With a C++ server, keep **one** definition source (recommend: keep the XML, or convert once to JSON) that both the C++ build and the Unity importer read; the importer produces `DefinitionCatalog` assets. Add a content hash to `Hello`/`MapInfo` so mismatches are detected (protocol decision - UNVERIFIED with the protocol agent).
- Addressables: not needed for phase 1 (catalog with direct references); add `com.unity.addressables` later if download size or live updates matter.

---

## 7. Interpolation approach

- **Remote entities**, phase 1 (parity): reproduce `Entity.OnTickPosition`/`Update` (`Entity.cs:177-237, 277-305`): velocity = delta / measured interval (clamp 30..200 ms), glide without overshoot, snap if > 3 tiles, stop animation 120 ms after arrival.
- Phase 2 (recommended once the C++ server sends tick numbers/timestamps): snapshot buffer per entity, render time = server time - interpolation delay (about 2 ticks; server tick ~50 ms per `Entity.cs:290` comment, UNVERIFIED actual rate), linear interpolation between bracketing snapshots, bounded extrapolation (<= 1 tick) on loss.
- **Local player**: client-side prediction with the shared movement rules (Domain `PlayerController`), send input/position with a sequence number; on server correction replay unacknowledged inputs (today: hard snap via `Goto`, `Goto.cs:23-34`). Needs protocol support (sequence/ack) - decision for the protocol/server agents.
- **Projectiles**: deterministic from spawn time + path; no interpolation needed beyond the shared path math.

---

## 8. Per-subsystem plan

Each subsection: Reference behavior / Current implementation / New architecture / Implementation status / Differences / Reason / Tests / Known issues.

### 8.1 Bootstrap and main loop
- **Reference behavior**: GL 3.3 window, settings load, startup plan (atlases, fonts, XML, account check, renderer), menus capped 60 fps, game uncapped/VSync/FpsCap.
- **Current implementation**: `Program.cs`, `Main.cs:47-239`, `WaW.Engine/GameWindow.cs:90-163`, `Loading/LoadPlan.cs`.
- **New architecture**: `Bootstrap` scene -> `AppRoot` (DontDestroyOnLoad) creates services (Settings, Net, AccountApi, Catalogs, Audio), runs async load steps with a progress bar, then the Title screen. Frame cap via `Application.targetFrameRate`/`vSyncCount` from settings.
- **Implementation status**: NOT STARTED
- **Differences**: no manual loop, no GPU timer, no renderer profile choice.
- **Reason**: Unity owns the loop and the graphics backend.
- **Tests**: EditMode test of the load-plan progress weighting (port `LoadPlan` logic if kept); PlayMode smoke: bootstrap reaches Title.
- **Known issues**: none yet.

### 8.2 Screens and flow
- **Reference behavior**: Loading -> Title -> Book (character list) -> Creation / Play -> Game -> (Death) -> Book; Portal from Title; fades and shared title backdrop; world-switch cover with milestones.
- **Current implementation**: `Display/ScreenManager.cs`, `Screens/*`, `Loading/LoaderFlows.cs`, `Loading/WorldLoad.cs`, `Game/Components/WorldLoadCover.cs`.
- **New architecture**: `ScreenStateMachine` in App; each screen a UI Toolkit document + controller; Game screen loads an additive `Game` scene; `WorldLoadCover` as a full-screen UI layer driven by the same milestones (Connected, MapInfo, PlayerSpawned, FirstFrame).
- **Implementation status**: NOT STARTED
- **Differences**: Title backdrop battle (`TitleBattle.cs`, 1183 lines) becomes a later cosmetic task (Timeline or simple scripted sprites).
- **Reason**: keep the flow, drop the bespoke UI engine.
- **Tests**: EditMode state-machine transitions (disconnect -> Book unless Death active, `LoaderFlows.cs:35-48`).
- **Known issues**: reference uses many static flags (`DeathScreen.Active`, `Client.IsReconnecting`) - make them session state.

### 8.3 Networking transport
- **Reference behavior**: TCP, little-endian length-prefixed frames, receive on thread pool, main-thread handling, 64 KB send buffer flushed per frame, 10 connect retries, disconnect -> map reset + version re-check + book.
- **Current implementation**: `Networking/Client.cs`, `SocketSendState.cs`, `SocketReceiveState.cs`, `SpanReader/Writer.cs`, `Packets/**`.
- **New architecture**: `ITransport` (Connect, Send(ReadOnlySpan<byte>), events), `TcpTransport` (background receive thread, `ConcurrentQueue<Packet>`), `PacketRouter` on main thread; protocol classes in `WaW.Protocol` matching the C++ server (owned by the protocol agent).
- **Implementation status**: NOT STARTED
- **Differences**: packet `Handle()` logic moves out of packet classes into the router/systems; no static `Client`.
- **Reason**: testability, WebGL transport swap, separation of wire format from gameplay.
- **Tests**: port `SpanReaderWriterTests`, `SendBufferTests`, `PacketIsolationTests`, `DeathPacketTests`, `StarterPacketTests` to EditMode against the new Protocol.
- **Known issues**: credentials in `Hello` (reference) - replace with token auth (server decision).

### 8.4 Client world state
- **Reference behavior**: static `Map` with entity dictionaries, enemy subset, tile chunks, quest id, local player.
- **Current implementation**: `Game/Map.cs`, `Game/MapTile.cs`, `Game/TileBuilder.cs`.
- **New architecture**: `WorldState` (Domain): `TileMap` (16x16 chunks, walkability, occupant), `Entities`, `Enemies`, `Players`, `Interactives`, `QuestTargetId`, `LocalPlayerId`; events for presentation.
- **Implementation status**: NOT STARTED
- **Differences**: instance, not static; no rendering inside.
- **Reason**: tests, clean world switch.
- **Tests**: tile bounds (port the off-by-one regression idea from `Map.cs:93`), add/remove entity bookkeeping, `ClearWorldObjects` completeness.
- **Known issues**: reference `TileMap.Clear` on network thread - not applicable when everything is applied on the main thread.

### 8.5 Local player movement and collision
- **Reference behavior**: rules R1-R6 in `ClientArchitecture.md` section 21 (speed formula, sinking, sliding, half-tile collision, Confused, camera-relative input), position sent once per `NewTick`.
- **Current implementation**: `Game/Objects/Player.cs:207-258, 508-767`; `Incoming/NewTick.cs:40-50`.
- **New architecture**: `PlayerController` (Domain) `Step(input, cameraAngle, dtMs)` with the same rules; `InputBridge` (Presentation) feeds it; send cadence decided with the server (per tick as today, or input commands).
- **Implementation status**: NOT STARTED
- **Differences**: TBD - the integer division in `GetMoveSpeed` (`Player.cs:697`) must be resolved with the server owner (bug or intended?).
- **Reason**: parity first, then authoritative server.
- **Tests**: golden tests from the reference formulas: speed at Speed 0/50/75, Slowed/Speedy, sink sequence, wall-slide cases from `IsValidPosition`.
- **Known issues**: reference movement integrates per render frame (frame-rate dependent rounding at borders).

### 8.6 Combat and projectiles
- **Reference behavior**: R7-R13: client shoots with DEX/ROF limits, numbers bullets 0..1999, simulates paths, tests walls and swept 0.5-radius hits, reports `EnemyHit`/`PlayerHit`; other players' bullets for show; damage numbers from server.
- **Current implementation**: `Player.cs:437-506, 711-715`, `Projectile.cs`, `ProjectilePaths/*`, `EntityUtils.cs:78-93`, `EnemyShoot.cs`, `ServerPlayerShoot.cs`.
- **New architecture**: `ProjectileSystem` (Domain) + `ProjectilePathMath` shared with C++ (same formulas, same test vectors); `ProjectileView` pool; hit authority per server decision (recommended: server-authoritative hits; client keeps local prediction only for sparks).
- **Implementation status**: NOT STARTED
- **Differences**: client damage roll dropped (`Player.cs:497` is unused anyway).
- **Reason**: authoritative C++ server.
- **Tests**: port `HitRadiusTests`; add path-math golden vectors shared with the C++ tests; bullet id wrap.
- **Known issues**: `EnemyShoot` path parsing and the per-path parameter set must match the new protocol.

### 8.7 Remote entity interpolation
- See section 7. **Current implementation**: `Entity.cs:177-237, 277-305`. **New architecture**: `EntityModel.Snapshots` + `Interpolator` (Domain). **Implementation status**: NOT STARTED. **Differences**: phase 2 adds a render delay. **Reason**: smoother, deterministic. **Tests**: glide/no-overshoot/snap>3 tiles; snapshot bracketing. **Known issues**: reference non-interpolated path is broken; do not port it.

### 8.8 Camera
- **Reference behavior**: orthographic top-down, 100 px/tile x zoom (0.5..5, default 1.7), free rotation with 80 ms ease or 45-degree snap, reset key, default angle per world entry, lowered view 2.5 tiles, thunder shake.
- **Current implementation**: `Game/Camera.cs`, `Player.cs:139-205`, `GameScreen.cs:230-239`.
- **New architecture**: `CameraRig` MonoBehaviour: orthographic camera, `orthographicSize = screenHeight / (2 * 100 * zoom)` in tiles (world unit = 1 tile), yaw rotation around the focus; rotation logic in Domain (`CameraAngleController`) so movement uses the same angle.
- **Implementation status**: NOT STARTED
- **Differences**: tilt dropped (reference tilt is 1.0 = off, `Camera.cs:16`).
- **Reason**: simpler; sprites billboard to camera yaw.
- **Tests**: EditMode snap-rotation stepping; ScreenToWorld round trip (PlayMode).
- **Known issues**: none.

### 8.9 Input
- **Reference behavior**: scancode bindings (`Settings.cs:63-112`), focus gating, HUD hit blocks firing, autofire toggle, chat hotkeys, potion and slot hotkeys.
- **Current implementation**: `Game/Components/UserInput.cs`, `Core/Settings.cs`.
- **New architecture**: Input System action maps `Gameplay` (Move 2D, Rotate 1D, Fire, AutoFire, Interact, Escape, ResetCamera, ToggleLowerView, UseSlot1-8, HealthPotion, MagicPotion, ZoomModifier+Scroll, MinimapZoom, Chat, Command, Tell, GuildChat, PartyChat, Options, PerfStats, ToggleBars, Fullscreen) and `UI`; rebinding UI with JSON overrides; UI Toolkit focus decides map enable/disable.
- **Implementation status**: NOT STARTED
- **Differences**: bindings by Input System path, migration of old scancode names not needed (fresh client).
- **Reason**: installed and active input backend.
- **Tests**: EditMode: default binding table matches the reference defaults.
- **Known issues**: `Special` (R) has no behaviour in reference (`UserInput.cs:175-177`).

### 8.10 Tile/world rendering
- **Reference behavior**: chunked tiles, random/mosaic variants, priority blending with 8x8 masks, edges/corners, animated water (Flow/Wave), water noise, lights, grade.
- **Current implementation**: `Map.cs:22-170, 316-334`, `MapTile.cs`, `TileBuilder.cs`, `Ground.vert/.frag`.
- **New architecture**: `TileChunkRenderer` per 16x16 chunk: one mesh with base quads + overlay quads (UV2 = mask), material `WaW/Ground` (Shader Graph or HLSL) sampling the game sprite atlas; rebuild on `TilesChanged`. Unity Tilemap not used (blending + rotation needs custom meshes).
- **Implementation status**: NOT STARTED
- **Differences**: water noise/reflections are phase 2.
- **Reason**: parity of look with lower complexity.
- **Tests**: port `GroundMosaicTests`, `AnimatedWaterTests` (pure math parts), `WorldRenderingAuditTests` where applicable.
- **Known issues**: blending rules live in `TileBuilder.cs`; port as pure C# in Domain/Presentation helper.

### 8.11 Entity sprites and animation
- **Reference behavior**: billboarded sprites, 8-sector facing relative to camera, Stand/Walk/Attack cells, idle flick, side view, size from `RealSize`/`Size` stat, name + HP bars, sorting by screen Y.
- **Current implementation**: `Rendering/Types/*`, `Utils/Texture.cs:58-96`, `Entity.cs:433-497`, `Player.cs:398-418`.
- **New architecture**: `SpriteEntityView` (SpriteRenderer or quad with `WaW/Sprite` material) rotated to camera yaw; `SpriteAnimator` (plain C# frame picker ported from `TextureFromFacing`); sorting via camera-space Y (transparency sort mode custom axis) or explicit `sortingOrder` from the reference formula.
- **Implementation status**: NOT STARTED
- **Differences**: no Animator Controllers (frame picking is data-driven).
- **Reason**: hundreds of entities, simple frame math.
- **Tests**: EditMode facing-sector and frame-index tables.
- **Known issues**: attack frame is double width (cell 5) - sprite pivot must account for it.

### 8.12 Walls and static props
- **Reference behavior**: walls are raised textured blocks (`TypeWall`, top texture), static objects baked per 16x16 area.
- **Current implementation**: `Rendering/Types/TypeWall.cs`, `TypeWallTop.cs`, `Game/StaticProps.cs`, `Model.vert/.frag`.
- **New architecture**: `WallView` cube mesh with side/top materials from the art catalog; static props as pooled views marked static or GPU-instanced.
- **Implementation status**: NOT STARTED
- **Differences**: no CPU baking.
- **Reason**: Unity batching.
- **Tests**: port `StaticPropsTests`' qualifying rule if kept.
- **Known issues**: none.

### 8.13 Shadows
- **Reference behavior**: contact spot + silhouette laid away from the sun, length from sky slant, blocked by solid tiles, MIN blending, shade on bodies under tall casters.
- **Current implementation**: `Render.Draw.cs:72-235`, `Shadow.vert/.frag`, `Rendering/ShadowGeometry.cs`.
- **New architecture**: phase 1 contact spots (quad under each view); phase 2 silhouette quads with a custom shader writing into a shade render texture via a URP renderer feature.
- **Implementation status**: NOT STARTED
- **Differences**: phase 1 omits silhouettes.
- **Reason**: lowest-risk first.
- **Tests**: port `ShadowGeometryTests` (pure maths).
- **Known issues**: none.

### 8.14 Lighting, day/night, weather, fog, bloom
- **Reference behavior**: UTC-driven 32-min day cycle and 7-min weather slots (deterministic hash), server override packet, colour grade per world, 12 point lights, lantern, rain/splash particles, Realm fog, lightning + shake, bloom with hero mask.
- **Current implementation**: `Game/DayNight.cs`, `Weather.cs`, `Lights.cs`, `Shelter.cs`, `Bloom.cs`, `Fog.*`, `Bloom.*`.
- **New architecture**: `DayNight`/`Weather` ported unchanged to Domain (pure); `WorldLighting` sets global shader properties (grade, sky colour, light arrays); URP Volume Bloom; fog full-screen pass feature; rain via ParticleSystem.
- **Implementation status**: NOT STARTED
- **Differences**: bloom hero mask dropped unless needed.
- **Reason**: built-in URP post-processing.
- **Tests**: port `DayNightTests`, `WeatherTests` verbatim (deterministic outputs).
- **Known issues**: if the server ever uses time of day for gameplay it must own the clock (rule R18/R19).

### 8.15 Particles and ambience
- **Reference behavior**: CPU particle generators (campfire, embers, fountain, mist, hit, ring, spark, sparker), 300-generator cap, fireflies/stars/motes.
- **Current implementation**: `ParticleEffects/*`, `Map.cs:281-292, 470-492`, `Game/Ambience.cs`, `FireGlow.cs`.
- **New architecture**: prefab ParticleSystems per effect, pooled; definitions `<Effect>` map to prefab keys.
- **Implementation status**: NOT STARTED
- **Differences**: visuals re-authored, not formula-ported.
- **Reason**: Unity tooling.
- **Tests**: PlayMode pool cap.
- **Known issues**: none.

### 8.16 Minimap and quest arrow
- **Reference behavior**: revealed-tile colours in a texture, entity markers, own marker shape/colour/turn, quest gold dot; quest arrow over target or pinned to screen edge with distance.
- **Current implementation**: `MinimapTexture.cs`, `Hud/Minimap.cs`, `Hud/MinimapLayer.cs`, `Core/MinimapIcon.cs`, `Hud/QuestArrow.cs`, `Incoming/QuestObjId.cs`.
- **New architecture**: `MinimapService` with a map-sized `Texture2D`; UI Toolkit element for markers; `QuestArrowView` using camera projection.
- **Implementation status**: NOT STARTED
- **Differences**: texture sized to the map, not 4096x4096.
- **Reason**: memory.
- **Tests**: port `MinimapIconTests`.
- **Known issues**: none.

### 8.17 HUD and in-game windows
- **Reference behavior**: plate, minimap frame tabs, bounty card, interact panel, item bar, power-up strip, chat, tabs + popups, overlays (options, bug board, admin, jukebox, starter reward), tooltips with comparisons, drag/drop inventory.
- **Current implementation**: `Game/Components/Hud/**`, `Options/**`, `Admin/**`, `BugBoard/**`, `Jukebox/**`, `Roles/**`, `Ui/**`.
- **New architecture**: UI Toolkit HUD document; each panel a `VisualElement` controller bound to Domain state; drag/drop via UI Toolkit manipulators sending `InvSwap`/`InvDrop`/`UseItem` intents.
- **Implementation status**: NOT STARTED
- **Differences**: layout rebuilt in UXML/USS on the same 1280x720 reference.
- **Reason**: replace UiLib.
- **Tests**: port `HudLayoutTests`, `AdminRulesTests`, `BugsAndTodoTests` (item card rules), `RoleClientTests`.
- **Known issues**: slot-fit rules must come from the shared `InventoryLayout` equivalent.

### 8.18 Menus: title, character book, creation, portal, death
- **Reference behavior**: see `ClientArchitecture.md` section 3.
- **Current implementation**: `Screens/**` (CharacterBook 1626+152+792 lines, ClassContainer 1183, PortalView 608+466+354, TitleBattle 1183).
- **New architecture**: UI Toolkit screens; HTTP via `AccountApi`; creation flow as a 3-step wizard; Portal as a later item.
- **Implementation status**: NOT STARTED
- **Differences**: animations simplified first.
- **Reason**: size of the reference code.
- **Tests**: port `CharacterListDataTests`, `PortalDataTests`, `RewardsDataTests`, `BugBoardDataTests`, `MusicNowDataTests`, `VersionCheckTests`, `LaunchLoginTests`.
- **Known issues**: none.

### 8.19 Options and settings
- **Reference behavior**: reflection-driven settings.xml; keys, camera, screen, minimap icon, audio, chat, particles, interpolation.
- **Current implementation**: `Core/Settings.cs`, `Game/Components/Options/**`.
- **New architecture**: `SettingsModel` (Domain, JSON), `SettingsStore` (Presentation/App, persistentDataPath), binding overrides JSON; options screen in UI Toolkit.
- **Implementation status**: NOT STARTED
- **Differences**: no renderer mode, no packet-logging setting in UI.
- **Reason**: drop GL options.
- **Tests**: EditMode round-trip and defaults.
- **Known issues**: never persist passwords.

### 8.20 Audio and music
- **Reference behavior**: music channel with crossfades, menu track choice, per-world libraries, Nexus jukebox from server HTTP, weather loops; SFX mostly unused.
- **Current implementation**: `WaW.Audio/*`, `Core/Audio.cs`, `Game/Music/*`, `Game/WeatherSound.cs`.
- **New architecture**: `AudioService` with AudioMixer groups, two music `AudioSource`s for crossfade, pooled SFX sources; `MusicPlan` logic ported to Domain.
- **Implementation status**: NOT STARTED
- **Differences**: `PlaySound` packet can finally be honoured if the server sends it.
- **Reason**: Unity audio.
- **Tests**: port `InGameMusicTests`, `MusicPlanTests`.
- **Known issues**: streaming of long MP3s: import as Streaming load type.

### 8.21 Content and definitions
- **Reference behavior**: runtime XML parse of all `Content/Xmls/*.xml`; art keys into atlases; same XML used by the server.
- **Current implementation**: `Assets/AssetParser.cs`, `Assets/XmlStructs/*`, `Content/Game.atlas`, `Content/Ui.atlas`.
- **New architecture**: see section 6.
- **Implementation status**: NOT STARTED
- **Differences**: edit-time import, deterministic duplicate handling (error, not first-wins).
- **Reason**: speed, validation, replaceable art.
- **Tests**: importer validation (unique ids, every art key resolves, every projectile id resolves).
- **Known issues**: parallel parse nondeterminism in reference.

### 8.22 Account / HTTP
- **Reference behavior**: form POSTs with credentials per call; launcher token env vars.
- **Current implementation**: `AppEngine/*`.
- **New architecture**: `AccountApi` (Net) with token auth; async.
- **Implementation status**: NOT STARTED
- **Differences**: token instead of password (server work).
- **Reason**: security.
- **Tests**: response parsing tests ported.
- **Known issues**: none.

### 8.23 Debug and performance
- **Reference behavior**: F5 readout, perf sections, packet logger, sundebug.
- **New architecture**: Profiler markers, debug overlay, Net packet log toggle.
- **Implementation status**: NOT STARTED
- **Tests**: port `FrameStatsTests`, `BudgetTests`, `DebugStatsLimiterTests` only if the classes are kept.
- **Differences / Reason / Known issues**: Unity Profiler replaces most of it.

### 8.24 WebClient
- See section 3. **Implementation status**: NOT STARTED (and not planned). **Tests**: none. **Known issues**: a later WebGL build needs WebSocket server support.

---

## 9. Open decisions (for the user / other agents)

1. Movement speed formula: keep the integer division behaviour or fix it (server and client must agree).
2. Hit authority: server-authoritative hits vs client-reported hits with validation.
3. Local movement: keep client-authoritative position + plausibility, or input-command prediction with reconciliation.
4. Definition source format shared by the C++ server and Unity (XML as today vs JSON) and content-hash handshake.
5. UI Toolkit everywhere vs uGUI for some pieces.
6. Day/night/weather: stay client-deterministic from UTC, or become server state.

---

## Implementation (2026-10-02)

| Assembly | Folder (WaW/Assets/Scripts) | Engine-free | Contents |
|---|---|---|---|
| WaW.Protocol | Protocol/ | yes | generated messages + byte I/O + framing |
| WaW.Net | Net/ | yes | `GameConnection` (TCP, reader/writer threads, main-thread `Poll`) |
| WaW.Domain | Domain/ | yes | `ContentCatalog` (reads Content/Definitions), `ClientTileMap`, `MovementRules` (bit-identical to the server), `LocalPlayerPredictor`, `PositionBuffer` / `ServerClock`, `ClientWorld` |
| WaW.Client | Client/ | yes | `ApiClient` (Account/API, bearer token), `GameSession` (connect, Hello, load/create, per-frame update, ping), `InputMapping` |
| WaW.Presentation | Presentation/ | no | `GameBootstrap` (flow), `WorldView` / `TileLayerView` (Tilemaps) / `EntityViewRegistry` (pooled views) / `CameraRig`, `PlayerControls` (Input System), uGUI screens built in code, `ArtCatalog` + placeholders |
| WaW.EditorTools | Editor/ | editor | `SceneBuilder` (WaW > Build Game Scene), `ContentBuildStep` (copies Content/Definitions into StreamingAssets for builds) |

Tests: EditMode (`Tests/EditMode`, 82, also run under .NET by `Tests/ClientCore`), PlayMode (`Tests/PlayMode`: the Game scene against real
API + C++ server processes, screenshot `TestResults/playmode-world.png`; async/HTTP runtime diagnostics).

UI: uGUI built in code for now (no theme assets needed) - sign in / register, characters, HUD with chat. UI Toolkit remains an option
when the real UI art arrives; the screens are thin and hold no game state.

Unity runtime findings (each pinned by a test):
1. Mono float precision: Unity's Mono kept a float intermediate at higher precision; `MovementRules` casts every intermediate with
   `(float)` (MovementTests.MatchesTheServerBitForBit failed in Unity only, case "mud", before the casts).
2. HttpClient in play mode: a request started on the main thread did not complete in Unity's play mode (AsyncDiagnosticsTests); `ApiClient`
   starts requests on a pool thread. Fire-and-forget UI flows go through `GameBootstrap.Run`, which logs and shows any failure.
3. Editor packages log errors of their own in batch mode (the AI Assistant relay); the PlayMode test judges by its assertions.
