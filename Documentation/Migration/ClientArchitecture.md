# Client Architecture (reference audit)

Scope: the OpenTK/OpenGL desktop client `WaW-Client/` and its WebAssembly build `WebClient/`, as they exist in the reference source.
All paths are relative to REF = `Reference - This Is The Source Being Ported To Unity/Warriors-and-Wizards-Testing/`.
Line numbers were read from the code on 2026-10-02. "UNVERIFIED" marks anything not confirmed in code.
Audit phase only: nothing in REF was modified and nothing was run.

Build version at audit time: `0.3.16` (`WaW-Client/WaWClient/Core/Settings.cs:30`).

---

## 1. Projects and what they are

| Project | Path | Role | Notes |
|---|---|---|---|
| WaWClient | `WaW-Client/WaWClient/` | The game executable (net10.0, OpenTK 5.0.0-pre.16) | `WaWClient.csproj`; post-build runs ContentBuilder over `Content/` |
| WaW.Engine | `WaW-Client/WaW.Engine/` | Window, main loop, GL wrappers (Shader, Texture, buffers, RenderTarget, Sampler) | `GameWindow.cs` is the loop |
| WaW.UiLib | `WaW-Client/WaW.UiLib/` | Own retained-mode 2D UI library (Flash-like display tree: Stage/Sprite/events/tweens) + its GL renderer | `Ui.vert/.frag` |
| WaW.Audio | `WaW-Client/WaW.Audio/` | OpenAL Soft audio engine on its own thread; WAV/MP3 (NLayer)/OGG (StbVorbis) decoding | |
| WaW.Common | `WaW-Client/WaW.Common/` | Color, atlas structs, small utils, `[Shader]` attribute | |
| WaW.ContentReader | `WaW-Client/WaW.ContentReader/` | Runtime loader for the built binary `.atlas` files and MSDF fonts | `ContentLoader` |
| WaW.ContentBuilder | `WaW-Client/WaW.ContentBuilder/` | Build-time tool: packs PNG sheets into atlases, builds fonts, copies XML/sound, FBX | driven by `Content/Content.xml` |
| WaW.ShaderSourceGen | `WaW-Client/WaW.ShaderSourceGen/` | Roslyn source generator embedding `AdditionalFiles/*.vert/.frag` as C# strings | |
| WaWClient.Tests | `WaW-Client/Tests/WaWClient.Tests/` | xUnit 2.9.3, 213 `[Fact]/[Theory]` (grep count) | references WaWClient directly |
| Common.Protocol | `Shared/Common.Protocol/` | Shared with the server: PacketId, InventoryLayout, LevelRules, Bounties, Roles, PowerUpList, NexusCats, WorldIds, ItemCategories | compiled into client and server |
| web | `WebClient/web/web.csproj` | The SAME client sources compiled to WebAssembly with browser shims | see section 20 |

---

## 2. Entry point and game loop

- `WaW-Client/WaWClient/Program.cs` `Main()`: invariant culture, `GCLatencyMode.SustainedLowLatency`, `Settings.LoadSettings()`, `new Main().Run()`; settings saved on process exit and on unhandled exception.
- `WaW-Client/WaWClient/Main.cs:31` `Main : GameWindow(new Version(3, 3), ...)` - requests an **OpenGL 3.3 core, forward-compatible** context (`WaW-Client/WaW.Engine/GameWindow.cs:33-39`).
- `Main.Initialize` (`Main.cs:47-102`): restores window position/size/mode from settings, GL state (blend SrcAlpha/OneMinusSrcAlpha, cull front, no sRGB), `Audio.Init`, `ContentLoader.Init("Content")`, `UiRender.ConfigureAndLoad` (creates the UiLib `Stage`), `DisplayManager.Init`, starts audio, wires `OnQuit`, `OnScreenChange` (frame cap), `InGameMusic.Init`, fullscreen toggle.
- `Main.LoadContent` (`Main.cs:105-116`): picks renderer (`ApplyRenderer`), loads `Ui.atlas`, then fades to `LoadingScreen(BuildStartupPlan())`.
- `BuildStartupPlan` (`Main.cs:121-171`, `Loading/LoadPlan.cs`): weighted steps - Game atlas (main thread), MyriadPro MSDF font, title logo sheet, title map, models/minimap/slices/condition effects, **XML parse on the thread pool** (`AssetParser.LoadAssetsAsync`), account check + character list (`AppRequests.Startup`), renderer init (`Render.FirstTimeInit`, texture-unit assignment), menu music.
- The loop: `GameWindow.Run` (`WaW-Client/WaW.Engine/GameWindow.cs:90-163`): `ProcessEvents` -> `Update(gameTime)` -> `Draw(gameTime)` -> `SwapBuffers` -> optional `AccurateSleep` to `TargetFrameTime`. `GameTime` is in **milliseconds** (`TotalMs`, `ElapsedMs`). A ring of 4 `GL_TIME_ELAPSED` queries measures GPU time (`GameWindow.cs:176-232`). Windows `timeBeginPeriod(1)`/`timeEndPeriod(1)` (`GameWindow.cs:289-296`).
- Frame pacing: `Main.SetGraphicOptions` (`Main.cs:220-239`) - menus capped at 60 fps; in game VSync or `FpsCap` or uncapped.
- `Main.Update`/`Draw` (`Main.cs:173-175`) forward to `Display/DisplayManager.cs` (`Update` line 24, `Draw` line 31): the current `Screen.Update/Draw` (world), then the UiLib stage update/draw (UI on top).

### Game-screen frame (`WaW-Client/WaWClient/Game/GameScreen.cs:165-254`)
1. `Client.Tick()` - flush outgoing, drain and `Handle()` every queued incoming packet (`Networking/Client.cs:186-195`).
2. `InGameMusic.Update()`.
3. Camera from last frame's player position (`CameraOnPlayer`, `GameScreen.cs:230-239`).
4. `UserInput.Update` (shooting), chat layer, notification layer, HUD, tutorial HUD toggle.
5. Fixed steps: `FixedStepper.Advance` (step 1000/60 ms, max 5 steps/frame, backlog dropped - `Game/FixedStepper.cs:13-14, 34-52`) -> `Map.FixedUpdate` (only projectile particle trails, `Map.cs:309-313`, `Projectile.cs:192-202`).
6. `Map.Update` (`Map.cs:260-307`): every entity `Update` + culling/depth, particle generators, projectiles (movement + **hit tests**).
7. `PartyData.Update`, `DevPerfTest.Update`.
8. Camera recomputed after movement; quest arrow, `Weather.Update`, lightning bolt, `Ambience.Update`.
9. Draw: `Render.SetShaderParams` -> `Map.Draw` (`Map.cs:316-424`) -> `Bloom.Draw` -> minimap texture upload.

---

## 3. Screens and screen manager

- `Display/DisplayManager.cs:12-22`: stage children in order: FadeScreen, `ScreenManager`, `OverlayManager`, `DialogManager`, `TooltipManager`.
- `Display/ScreenManager.cs`: one current `Screen` (a UiLib `Sprite` with virtual `Update/Draw`, line 109). `FadeToScreen` (line 56) fades a black sheet over the old screen, swaps, fades away (game screen enters opaque); `SwapScreen` (line 83) for the menu screens that share `TitleBackdrop`. Alt+Enter / F11 fullscreen (line 101). Dispatches `Main.OnScreenChange` (Menu/Game) for frame-cap changes.
- `Display/OverlayManager.cs:35-66`: one modal `Overlay` at a time, dimmed background, alpha tweens, centred on the 1280x720 design canvas scaled by `Stage.ScreenScale`.
- Screens (`WaW-Client/WaWClient/Screens/`):
  - `LoadingScreen` / `LoaderScreen` - logo + progress bar driven by a `LoadPlan` of real steps.
  - `TitleScreen` (`TitleScreenBase`, shared `TitleBackdrop.Shared`, animated logo, `TitleBattle` - a purely cosmetic AI skirmish simulation in the background, 1183 lines).
  - `CharacterListScreen` -> `Components/CharacterList/CharacterBook.cs` (1626 lines + `.FastTravel.cs`, `.Rewards.cs`): profile, character slider, fast travel, inbox, daily spin/gift, graveyard. All data via HTTP (`AppEngine/AppRequests.cs`).
  - Character creation: `Components/Containers/ClassContainer.cs` (step-by-step: Class -> Style/skin -> Initiation/role), sets `GlobalData.CharacterType/SkinType/RoleId` then enters `GameScreen`; `MapInfo.LoadOrCreate` sends `Create` (`Networking/Packets/Incoming/MapInfo.cs:66-83`).
  - `RolePickView` (legacy role-less characters), `LoginContainer`, `RegisterContainer`, `AccountOverlay`.
  - `PortalScreen` -> `Components/Portal/PortalView*.cs` (in-game website: leaderboards, guilds, players via `PortalRequests`; wiki pages built from the client's own XML).
  - `DeathScreen` (stats + fame roll-in from the `Death` packet).
  - `Game/GameScreen.cs` (the world).
- Flow helpers: `Loading/LoaderFlows.cs` (`ToCharacterList` after disconnect), `Loading/WorldLoad.cs` milestones (Connected, MapInfo, character accepted, PlayerSpawned, FirstFrame) driving `Game/Components/WorldLoadCover.cs` (black cover, five-step teleport on world switch).

---

## 4. Rendering architecture

### 4.1 GL layer
- `WaW.Engine/Graphics`: `Shader` (+`ShaderHelper`), `Texture` (TexImage2D, MaxLevel 0), `Sampler` (fixed texture units), `RenderTarget`, `IndexBuffer/VertexBuffer/InstanceAttributeBuffer` (orphan + BufferSubData every write), `StorageBuffer<T>` = RGBA32UI **data texture** emulating an SSBO (GL 3.3), `UniformBuffer`, `GraphicsProfile` (Compatible vs Fast/instanced; `Main.ApplyRenderer` `Main.cs:208-218`, option `Settings.RendererMode`).
- Fixed texture-unit table (CLAUDE.md "TEXTURE UNITS"; confirmed in `Main.cs:156-165`, `Bloom.cs:24`, `SightMap.cs:24`): 0 game atlas, 1/2 UI atlas (nearest/linear), 3 minimap, 4/5 title, 6 font, 8 sight map, 10-12 bloom, 13-14 data textures, 15 uploads.

### 4.2 Shaders (`WaW-Client/WaWClient/AdditionalFiles/` + `WaW-Client/WaW.UiLib/AdditionalFiles/`; all `#version 330`)
| Shader | Purpose |
|---|---|
| `Ground.vert/.frag` | Tile chunks: atlas tile + up to 8 blend overlays (alpha masks `tileAlphaBlend`), edge/corner strips, Flow/Wave animated water, procedural water noise, sky reflection, 12 point lights (`LightPos[12]`), sight/light occlusion texture, colour grade (GradeShadow/Light/Sat), shore wetness |
| `Object.vert/.frag` | Billboarded sprites (entities, projectiles, name/HP bars, in-world text via MSDF): camera billboard, `SpriteStretch` for tilt, flat-on-ground flag, lights per sprite, shade-from-casters, grade, two passes (opaque, transparent) |
| `Model.vert/.frag` | Raised wall blocks and baked static props: per-vertex sort depth from ground point, face normals lit by sky light, flip-book/flow faces, lights, grade |
| `Shadow.vert/.frag` | Contact spot + silhouette shadow per caster (12 verts from two 16-byte records), sun slant uniform `Sun`, drawn into a white-cleared screen target with MIN blend |
| `Particle.vert/.frag` | Particles (sparks, trails, rain streaks a=2..3, splash rings a=4..5, soft glow a=6..7) read from a data texture |
| `Fog.vert/.frag` | Full-screen Realm ground fog (value noise, ground point reconstructed from inverted camera) |
| `Bloom.vert/.frag` | Pass 0 bright-pass (hero masked out), 1-2 separable blur, 3 additive composite, 4 multiply the shadow picture onto the screen |
| `Ui.vert/.frag` (UiLib) | All UI quads: atlas sprites, nine-slice, MSDF text with outline, colour rects/ellipses/cut-edge rects; uses `dFdx`/`textureGrad` |

Shader C# wrappers are generated by `WaW.ShaderSourceGen` from `[Shader("Name")]` partial properties (`Rendering/Render.cs:29-35`); compiled in `Render.FirstTimeInit` (`Render.cs:72-97`).

### 4.3 Draw order (`Game/Map.cs:316-424`)
1. Tiles: 7x5 chunks of 16x16 around the camera (`Map.cs:174-178, 326-332`); each chunk mesh rebuilt only when dirty (`Map.cs:136-144`, `Rendering/TileChunkMesh.cs`).
2. Shadows (entities of `ModelType.PbObject`, static props, projectiles) into the shade picture.
3. Particles (depth test on).
4. Models per `ModelType` (walls), then baked `StaticProps` (16x16 area meshes, `Game/StaticProps.cs:19`; `WAW_NO_STATIC_BAKE=1` disables, line 37).
5. Sprites (`PbObject` entities + projectiles + `/sundebug` marks), depth-sorted (`Render.FlushBufferEntity`).
6. `Weather.Draw` (fog, rain), `FireGlow.Draw`.
7. After `Map.Draw`: `Bloom.Draw` (`GameScreen.cs:245`).

Depth/sort: a sprite's sort value is `0.5 + 0.4 * screenY(ground point) + jitter` (`Entity.UpdateVisibility`, `Entity.cs:240-250`); culling by `CullRules` circle (`Game/CullRules.cs:11-21`).

Entity render types (`Rendering/Types/`): `TypeGameObject` (sprite + optional animation flip-book, `<Frames>/<Fps>`), `TypePlayer` (hero layout, attack frame twice as wide), `TypeWall` + `TypeWallTop` (raised block; `WallModel` Cliff/Forest variants), `TypeGroundObject` (`DrawOnGround`), `TypeNullObject`; sub-renderers `TypeName`, `TypeHpBar`, `TypeBar`, `TypeEffects`. Selected in `Entity.GetRenderType` (`Entity.cs:135-164`).

Dead rendering code still present: `Render.DrawCrossedCards`, `DrawFlatStack`, `BakeCrossedCards`, `BakeFlatStack` (`Rendering/Render.Draw.cs:330, 369`; `Render.Baked.cs:31, 62`) have no callers; `ModelType` still enumerates the old 3D model set (`Assets/ModelData.cs:12+`); `MapTile.ShoreCoverage` is computed (`TileBuilder.cs:38`) but nothing reads it.

---

## 5. Camera

`Game/Camera.cs`:
- Orthographic, base zoom 100 px/tile x `Settings.CameraZoom` (`Camera.cs:9, 29`; zoom 0.5..5, default 1.7 - `Settings.cs:55-56, 142`).
- **Rotation** around the player: `Settings.CameraAngle` (live) vs `Settings.DefaultCameraAngle` (option; applied on world entry `Map.cs:620` and reset key X). Q/E rotate; free spin with 80 ms ease (`Player.cs:158-173`, `RotateSpeed` 0.003 rad/ms) or 45-degree snap mode (`Player.cs:139-205`). Note: camera rotation is driven from inside `Player.HandleRelativeMovement` (`Player.cs:207-209`).
- **Tilt**: `GroundDepthScale = 1f`, `HeightScale = 1f` (`Camera.cs:16-17`) - i.e. straight top-down; the tilt machinery remains (`SpriteStretch` in shaders).
- Focus: player position, or 2.5 tiles "up the screen" with `LowerPlayerView` (`GameScreen.cs:230-239`, `Settings.cs:201-202`), plus thunder shake (`Weather.ShakeOffset`).
- `ScreenToWorld` (mouse aim, `Camera.cs:50-68`), `WorldToScreen` (quest arrow, lightning, bloom hero mask).

---

## 6. Input

- OpenTK events -> UiLib `Stage` keyboard/mouse events -> `Game/Components/UserInput.cs` (a `Sprite` listening on the stage).
- Bindings are `InputSetting(Scancode)` in `Core/Settings.cs:63-112`: WASD move, Q/E rotate, X reset angle, Z centre/lower view, O options, Y autofire toggle, R special (TODO, no effect), I interact, F escape to Nexus, Enter chat, `?` command, Tab tell, G guild, P party, PgUp/PgDn chat history, C/V health/magic potion, 1-8 inventory slots, F5 perf stats, B switch tabs (UNVERIFIED use), H toggle status bars, F11 fullscreen. Saved by scancode name in settings.xml; `ApplyKeyDefaults` migrates old defaults (`Settings.cs:307-320`).
- Movement keys -> `Player.SetRelativeMovement(rotate, x, y)` (`UserInput.cs:114-123`); relative to the camera angle.
- Fire: left mouse held (ignored when over the HUD, `UserInput.cs:74-79`) or autofire -> `Player.Shoot(angle)` every frame (`UserInput.cs:101-112`).
- Shift+wheel = zoom (saved immediately), wheel = minimap zoom (`UserInput.cs:125-138`).
- Focus gating: window focus + "manual focus" (cleared while overlays/chat are open) (`UserInput.cs:68-72`).
- Escape key (F): client refuses in Nexus or while reconnecting, sends `Escape`, sets `Client.IsReconnecting` (`UserInput.cs:178-184`).

---

## 7. UI (WaW.UiLib) and HUD

### 7.1 UiLib
A Flash/Starling-style display tree: `Core/Stage.cs`, `Core/Sprite.cs` (+ `.Bounds/.Dragging/.Rendering`), `Core/DisplayContainer.cs`, `Core/EventManager.cs` (bubbling mouse/keyboard events, EnterFrame broadcast), built-ins (`ColorRect`, `NineSliceRect`, `CutEdgeRect`, `Ellipse`, `ObjectRect`, `SimpleText`, `TextInput`, `Container` with clip), `Extra/GTween` + `Timer` + `ColorTransform`, `Signals/Signal` (weak-reference listeners). Renderer `Rendering/SpriteRender.cs` with a ring of `BufferSetCount = 32` buffer sets (`SpriteRender.cs:23`). Design canvas 1280x720 scaled by `Stage.ScreenScale`.

### 7.2 HUD (`Game/Components/Hud/`, root `HudView.cs`)
- Top-left `PlayerPlate` (name, HP/MP/XP bars, gold, account fame, character fame bar, `Sundial`).
- Top-right minimap frame with side tabs MAP / FPS (`DebugStats`) / PLAYERS (`NearbyPlayersPanel`); `BountyCard` under it; `InteractPanel` (portal, loot bag/container, storage, bug board, jukebox, cat panels in `Panels/`).
- Bottom: `Inventory/ItemBar` (HP stack | gear | MP stack, inventory, backpack; locked backpack without `HasBackpack`), `PowerUpFrame` strip, `ChatBox` bottom-left.
- `HudTabs` (stats, menu, news, skin, role, admin - admin only for staff, `HudView.cs:165`) opening `HudPopup` windows: `StatsPopup`, `NewsScroll`, `SkinPopup`, `RolePopup`, `FeedCatWindow`.
- `InitiationTips` (tutorial guide), `QuestArrow` (section 13), `ChatLayer`/`SpeechBubble`, `NotificationLayer` (bounded floating status/damage text).
- Overlays: `Options/OptionsView` (tabs via `OptionTabView`, option types Choice/KeyMapper/Slider), `BugBoard/BugBoardView`, `Admin/AdminDashboardView` (staff; sends chat commands, server checks rank), `Jukebox/JukeboxView`, `Roles/StarterRewardView`.
- `Inventory/ItemTile.cs`: drag/drop -> `InvSwap`, drop over world -> `InvDrop`, double-click/shift-click on bags, slot-fit test via shared `InventoryLayout.SlotFits` (`ItemTile.cs:465, 547`), `UseFirst("Health Potion")` (`ItemTile.cs:579`). Tooltips `Ui/Components/Tooltips/EquipmentToolTip.cs` (item comparison).

---

## 8. Audio

- `WaW.Audio/AudioEngine.cs`: OpenAL Soft (Silk native), dedicated background thread `WaW.Audio.Engine` with a command queue; static + streamed buffers; WAV, MP3 (NLayer), OGG (StbVorbisSharp). OpenAL library loaded by path (`InternalUtils.GetAudioBinaryPath`).
- `Core/Audio.cs`: one `SingleTrackChannel` (music with crossfades) + one `SfxChannel` (one-shots + loops).
- Music: `Game/Music/InGameMusic.cs` - menu track (`Settings.MenuMusic`), world libraries shuffled locally (`WorldMusic.g.cs`), shared Nexus jukebox polled over HTTP (`AppRequests.GetMusicNow`, `/music/now`), `MusicPlan` overlap scheduling.
- SFX actually used: only weather loops/thunder (`Game/WeatherSound.cs`). The `PlaySound` packet handler is empty (`Networking/Packets/Incoming/PlaySound.cs:19`). Oryx `.wav` sounds are shipped but nothing plays them (CLAUDE.md, consistent with code grep).

---

## 9. Animation (sprite sheets, AnimatedTexture)

- XML `<AnimatedTexture><File>sheet</File><Index>n</Index>` -> `Atlas.GetAnimationAtlasData` -> `AnimationAtlasData { FaceRight[], FaceDown[], FaceUp[] }` (`WaW.Common/Structs/AtlasStructs.cs:99-103`); left = mirrored right.
- Frame choice `Utils/Texture.cs:70-96` `TextureFromFacing`: facing relative to the camera angle split into 8 sectors -> row; Stand = cell 0, Walk = cells 1-2, Attack = cells 4-5 (cell 5 drawn double width = "attack frame"). `FrameFromFacing` cell 3 = idle flick.
- Timing: local player walk period `3.5 / moveSpeed` (`Player.cs:409`), others `0.5/(|v|*4)` rounded up to 400 ms multiples (`Entity.cs:450-451`), attack period `Entity.AttackPeriod = 300` ms for monsters (`Entity.cs:26`), player attack period from DEX + weapon `RateOfFire` (section 12).
- `<SideView/>` (cats) only left/right on screen (`Entity.cs:476-481`); `<IdleFlick/>` random second idle pose per object id seed (`Entity.cs:489-497`).
- Non-character flip-books: `<Frames>/<Fps>` in `TypeGameObject` / `Rendering/FlipBook.cs`; ground flip-books via `GroundProperties.Frames/Fps` into `TileData.Temp`.

---

## 10. World, tiles, map

- `Game/Map.cs`: **static** world state - `Width/Height/Name/DisplayName/Difficulty/Seed/Background/AllowPlayerTeleport/ShowDisplays` (`Map.cs:188-196`), dictionaries `Entities`, `Players`, `InteractiveObjects`, `Enemies` (hit-test subset), `Projectiles` list, `ParticleGenerators` (max 300, `Map.cs:472`), `Particles[30000]`, `LocalPlayer/LocalPlayerId`, `QuestId`, render storage per `ModelType`.
- `TileMap` (`Map.cs:22-170`): 16x16 chunks created lazily; per chunk tile array + cached `TileData` + GPU mesh rebuilt on dirty; meshes freed via a graveyard on the main thread (`Map.cs:70-91`) because `Map.Reset` can run on the network thread.
- `Game/MapTile.cs`: ground type, `GroundProperties`, `OccupiedObject` (static objects register themselves on their tile, `Entity.MoveTo` `Entity.cs:262-268`), `IsWalkable` (`MapTile.cs:126-128`: not `NoWalk` and no `OccupySquare` occupant), random/mosaic texture choice (`GroundMosaic`), animate data.
- `Game/TileBuilder.cs`: neighbour blending by `BlendPriority` using 8x8 alpha masks, edges/corners, composite tiles, shore marking.
- Tile changes arrive in `Update` packets -> `Map.SetTileData` rebuilds the 3x3 neighbourhood (`Map.cs:438-468`).
- Unknown object types fall back to the Red Pillar `0x017e` (`Networking/Packets/Incoming/Update.cs:76-79`).

---

## 11. Minimap

- `Game/Components/MinimapTexture.cs`: a single 4096x4096 RGBA texture (+ a 4096x4096 `Color[]` CPU copy, ~64 MB RAM) where each revealed tile writes one pixel (ground dominant colour, or a static occupying object's colour, `MapTile.cs:39-45`); dirty-rect upload once per frame (`GameScreen.cs:247`).
- `Hud/Minimap.cs` + `Hud/MinimapLayer.cs`: clipped view, zoom by mouse wheel, markers for entities (`Core/MinimapIcon.cs`, own marker shape/colour/turn options), quest monster gold dot (`MinimapLayer.cs:98`).
- Fog of war = only tiles the server has sent are drawn ("uncovered").

---

## 12. Effects, particles, lights, day/night, weather, bloom, shadows

| System | File | What it does | Gameplay meaning |
|---|---|---|---|
| Particles | `ParticleEffects/*.cs` (Campfire, Embers, Fountain, Mist, Hit, Ring, Spark, Sparker) | CPU generators filling `Map.Particles`; object `<Effect>` picks one (`ParticleEffect.FromProperties`) | none |
| Projectile trails | `Projectile.FixedUpdate` | 3 `SparkEffect`s per fixed step | none |
| Hit spark | `Projectile.Struck` | `HitEffect` on every local hit | none (instant feedback) |
| Condition effects | `Game/ConditionEffect.cs` | bucketed bit flags from stat `Condition1`; icons | movement/shoot modifiers (section 15) |
| Lights | `Game/Lights.cs` | 12 nearest `<Light>` sources + player lantern at night, flicker | none |
| Sight map | `Game/SightMap.cs` | CPU shadowcast per light (r=8) and, in line-of-sight worlds, from the player (r=22) into a 256x68 texture; `LineOfSightWorlds` is empty (`SightMap.cs:27`) | visibility (currently inactive) |
| Day/night | `Game/DayNight.cs` | **32-minute cycle from the UTC clock** (`CycleMinutes` line 34, `Phase` 62-65); colour-grade keys Day/Golden/Dusk/Night/Dawn; per-world nights; sky light direction for shadows/faces; only worlds Nexus/Realm/Campsite have sky (line 35) | none on the client; server can force via `WeatherState` |
| Weather | `Game/Weather.cs` | **7-minute slots hashed from UTC** (`SlotMinutes` 20, `Hash` 68-73, `SkyAt` 85-107): rain 30 %, storms 35 % of rain, overcast, wind, lightning buckets of 20 s; rain/splash particles, Realm fog, thunder shake; `Shelter.cs` hides rain on walls/rooms | none (cosmetic) but globally deterministic |
| Ambience | `Game/Ambience.cs` | fireflies, stars on water, dust motes | none |
| Fire glow | `Game/FireGlow.cs` | soft additive embers/smoke | none |
| Shadows | `Rendering/Render.Draw.cs` + `Shadow.*`, `Rendering/ShadowGeometry.cs` | silhouette + contact spot, length from sun slant, blocked by solid tiles (`ShadowReach`), objects darkened under tall casters (`ShadeAt`) | none |
| Bloom | `Game/Bloom.cs` | quarter-res bright pass, blur, add; hero masked out | none |
| Lightning | `Screens/Components/LightningBolt.cs` via `Weather.OnStrike` | bolt + light burst | none |

Overrides: env `WAW_TIME`, `WAW_WEATHER` (local), server `WeatherState` packet sets `DayNight.Server` / `Weather.Server` (`Incoming/WeatherState.cs:23-26`). Options: Weather Effects, Bloom, Eye Candy Particles (`Settings.cs:186-189`).

---

## 13. Asset loading

### 13.1 Build time
`WaW.ContentBuilder` (post-build of WaWClient, `WaWClient.csproj` PostBuild target) reads `WaW-Client/WaWClient/Content/Content.xml`:
- `Copy`: `Xmls/*.xml`, `Title/*.png`, `Sound/**`
- `Font`: `Fonts/*.msdf` (MSDF atlas generation; needs Wine on Linux per `WaW-Client/README.md`)
- `Fbx`: `Objects/*.fbx` (no `Objects` folder exists in Content today)
- `Atlas`: `Game.atlas`, `Ui.atlas` - XML recipes listing `<Image name w h>` / `<Animated name w h group>` sheets; packed with stb_rect_pack into fixed 4096x4096 pages (`WaW.Common/AtlasConfig.cs`, padding 1 px), written as a custom binary file (`Builders/AtlasBuilder.cs:126-133`). Overflow prints "Failed to add" and the sprite silently disappears.
- Hash cache: `Content/bin/content.hash` (`HashManager.cs`).

### 13.2 Run time
- `WaW.ContentReader/ContentReader.cs:6-26` `ContentLoader.LoadAtlas/LoadFont/LoadTexture` from `<exe dir>/Content`.
- `Atlas` keeps `name -> AtlasData[]` (static cells) and `name -> AnimationAtlasData[]`, dominant colours (minimap), and for the game atlas a 1-bit opaque mask (2 MB) used to measure shadows (`Atlas.cs`).
- **Client XML**: `Assets/AssetParser.cs:19-72` parses `Content/Xmls/Ground.xml` (`<Ground>`) and **every** `Content/Xmls/*.xml` for `<Object>` (Containers, Equip, Ground, NPCs, Objects, Players, Projectiles, StaticObjects) in parallel into `GroundLibrary` / `ObjectLibrary` (`TypeToObjectProps`, `TypeToTextureData`, `IdToObjectType`, `TypeToClassProps`, `TypeToSkins`, `TypeToItem`). Duplicate ids: first `TryAdd` wins, order not deterministic (parallel). XML structs: `Assets/XmlStructs/{GroundProperties,ObjectProperties,ItemDesc,PlayerProperties,ProjectileProperties}.cs`.
- **The server reads the same files**: `WaW-Server/Common/Common.csproj:143-160` links `..\..\WaW-Client\WaWClient\Content\Xmls\*.xml` as `Resources\Xml\Data\Xmls\*.xml`. The client folder is the single source of truth for definitions today.
- Art references in XML are `<Texture><File>atlasName</File><Index>n</Index>` - i.e. already a key into the atlas, not a file path (`AssetParser.cs:96-147`).
- Fonts: one MSDF family `Fonts/MyriadPro/MyriadPro.msdf` (`Main.cs:132`) with PixelParchment / Signature faces in the group (CLAUDE.md).
- `SliceLibrary` (nine-slice data), `ModelData.Load` (wall/prebuilt meshes), `ConditionEffects.Init`, `MinimapTexture.Init` in the startup plan.

---

## 14. Client state

| State | Where | Notes |
|---|---|---|
| World | `Game/Map.cs` (static) | entities, tiles, projectiles, particles, quest id; `Map.Reset` on disconnect/MapInfo, `ClearWorldObjects` on Reconnect |
| Session/global | `Data/GlobalData.cs` | type-keyed `ConcurrentDictionary<Type, IGlobalData>` (LoginData, AccountData, CharacterListData, ...), plus statics `SelectedCharacterId`, `CharacterType`, `SkinType`, `RoleId` |
| Party/nearby | `Game/PartyData.cs` | ignored/locked lists (`EditAccountList` packets), nearby players sorted every 500 ms |
| Settings | `Core/Settings.cs` | reflection over static `InputSetting`/`ValueSetting<T>` fields -> `%LocalAppData%\WaWClient\settings.xml`; `account.xml` stores username + **Base64 password** (`Settings.cs:352-423`) |
| Account/HTTP data | `Data/*.cs` (AccountData, CharacterListData, NewsFeedData, PortalData, RewardsData, BugBoardData, MusicNowData, FastTravel, RoleState, ServerData) | filled by `AppEngine/AppRequests.cs` |
| Signals | `Main.OnQuit/OnScreenChange/OnFullscreenToggle`, `Map.OnPlayerUpdate`, `Minimap.OnNewMap`, `ChatBox.OnChatOpen`, `Panel.OnInteract` | static UiLib `Signal`s |

---

## 15. Networking (brief; protocol covered elsewhere)

- `Networking/Client.cs`: static; TCP (`TcpClient`, NoDelay) to `Settings.GameServerAddress:SelectedGameServerPort` (default 127.0.0.1 / VPS via `TARGET_VPS`, port 2050; `Settings.cs:35-50`). Connect retries 10x on refusal (`Client.cs:61-96`). Receive with `SocketAsyncEventArgs` on the thread pool, parse into pooled packet objects, `ConcurrentQueue` (`Client.cs:136-184`); main thread `Tick` sends pending + handles all (`Client.cs:186-195`). Send buffer 64 KB, packets dropped (counted) when full (`Client.cs:230-246`). `Disconnect` resets the map and returns to the book (`Client.cs:248-269`).
- Framing (CLAUDE.md, E2E notes): int32 little-endian length incl. 5-byte header, byte id, body; UTF = uint16 LE length + bytes (`SpanReader/SpanWriter`).
- `Hello` sends **username + password (or launcher token) in clear** on every connect/reconnect (`Client.cs:271-280`, `Incoming/Reconnect.cs` `EnterNewWorld`). `Reconnect` re-sends `Hello` on the same socket.
- HTTP to the account server (`AppEngine/AppEngineClient.cs`, `AppRequests.cs`): form POSTs with username + password on every call; endpoints `/account/verify|register|purchaseCharSlot|purchaseSkin`, `/char/list|delete|chooseRole`, `/board/*`, `/inbox/*`, `/daily/*`, `/news/feed`, `/music/*`, `/app/version` (`VersionCheck`), Portal public API (`PortalRequests.cs`). Launcher sign-in: env `WAW_LAUNCH_USER` / `WAW_LAUNCH_TOKEN` (`AppEngine/LaunchLogin.cs`).
- Incoming packets with **empty handlers** (read but ignored): `AllyShoot`, `Aoe`, `BuyResult`, `ClientStat`, `Damage`, `File`, `GlobalNotification`, `GuildResult`, `InvitedToGuild`, `NameResult`, `Pic`, `PlaySound`, `ShowEffect`, `TradeAccepted/Changed/Done/Requested/Start`.

---

## 16. Prediction, interpolation and local simulation (what gameplay the client simulates)

### 16.1 Local player: client-authoritative movement with server plausibility checks
- Integrated every render frame in `Player.HandleRelativeMovement` (`Player.cs:207-258`): velocity = `moveSpeed` in the direction `cameraAngle + atan2(input)`; ice-like sliding when `GroundProperties.SlideAmount > 0`.
- Collision: `ModifyMove` sub-steps moves larger than 0.4 tiles (`Player.cs:508-542`), `ModifyStep` slides along half-tile borders (`548-620`), `IsValidPosition` (`622-676`): target tile must be walkable (`MapTile.IsWalkable`), and neighbouring tiles that are void / `0xFF` / `FullOccupy` block the half of the tile next to them.
- Position reported in `Move` **once per server tick** inside the `NewTick` handler (`Incoming/NewTick.cs:40-50`), followed by `Player.OnMove` which applies ground `Speed` multiplier and `Sinking` (max sink 18, `Player.cs:735-767`).
- Server correction: `Goto` teleports the local player and acks (`Incoming/Goto.cs:23-34`); server anti-cheat snaps impossible moves (CLAUDE.md; `WaW-Server/GameServer/Game/Systems/Combat/PlausibilityRules.cs:9-20` checks against the max speed 0.0096 tiles/ms with slack).
- No input sequence numbers, no reconciliation/replay: the client simply jumps to the `Goto` position.

### 16.2 Remote entities: interpolation
- `Entity.OnTickPosition` (`Entity.cs:277-305`): on each `NewTick`/`Update`, velocity = (new - current) / measured interval (clamped 30..200 ms, default 50 ms); jumps > 3 tiles snap.
- `Entity.Update` (`Entity.cs:177-237`) with `Settings.MovementInterpolation` (default true, `Settings.cs:204`): glide toward the latest server position at that velocity, never overshoot; stop walking animation 120 ms after arrival. No render-delay buffer (it chases the newest sample).
- The non-interpolated branch extrapolates from `PositionAtTick` using `LastTickUpdateTime`, but `OnTickPosition` is always called with `tickTime = 0, tickId = 0` (`NewTick.cs:63`, `Update.cs:103`) and `Map.LastTickId` is never assigned (only reset, `Map.cs:603`) - that branch is broken (extrapolates from time 0). Option exists in Options UI (UNVERIFIED which tab).

### 16.3 Combat simulated on the client
- Shooting `Player.Shoot` (`Player.cs:465-506`): blocked by `Stunned`/`Paused`; weapon = slot 0 or the class starting weapon in the Tutorial map (`InitiationWeapon`, `Player.cs:452-456`); rate limit `AttackPeriod = 1/AttackFrequency * 1/RateOfFire`, `AttackFrequency = 0.0015 + DEX/75 * (0.008-0.0015)` per ms, `Dazed` -> min, `Berserk` x1.25 (`Player.cs:437-448`); `NumProjectiles` spread by `ArcGap`; one `PlayerShoot { Angle }` per bullet; local bullet numbers from `GetBulletId` wrapping at 2000 (`Player.cs:711-715`, `Projectile.cs:79-80`). A client damage roll is made (`Player.cs:497`) but never sent or shown.
- Projectile flight: `ProjectilePath` (+ `LinePath`, `WavyPath`, `AmplitudePath`, `CirclePath`, `BoomerangPath`, `AcceleratePath`, `DeceleratePath`, `ChangeSpeedPath`, `CombinedPath`, segments) evaluated by elapsed time (`Projectile.cs:150-190`). Enemy bullets receive their path in `EnemyShoot` (`Incoming/EnemyShoot.cs:49`); player bullets use the XML path (`projDesc.Path.Clone()`).
- Wall test: bullet dies entering void/`0xFF` tile or a tile whose occupant is `EnemyOccupySquare`, or `OccupySquare` without `PassesCover` (`Projectile.cs:226-244`).
- **Hit test on the client**: swept circle, radius 0.5 (`Projectile.cs:76`, `EntityUtils.SweepHitT` `EntityUtils.cs:78-93`), run per render frame in `Projectile.Update` (`Projectile.cs:179`). My bullets vs `Map.Enemies` -> `EnemyHit { BulletId, TargetId }`; enemy bullets vs the **local player only** -> `PlayerHit { BulletId, ObjectId }`; multi-hit bullets remember targets for their lifetime (`Projectile.cs:250-304`). Other players' bullets (`ServerPlayerShoot`) are simulated for show only (`Reports = false`).
- Damage numbers come only from the server's `Notification` packets (`NotificationLayer`).
- Enemy shots: `EnemyShoot` spawns `NumShots` bullets with ids `WrapBulletId(First + i)`, angle `Angle + AngleInc*i` (`EnemyShoot.cs:68-74`).

### 16.4 Other client-computed state
- Facing/animation, camera, culling, minimap reveal, quest arrow placement, nearby-player sorting, day/night/weather (from UTC), bounty card display from stat `BountyState`, power-up remaining time (`PowerUpsActive` ms-left -> local end time, `Player.cs:372-379`).

---

## 17. Configuration

| Item | Where | Notes |
|---|---|---|
| settings.xml | `%LocalAppData%\WaWClient\settings.xml` (`Settings.cs:213-219`) | every public static setting field by name; saved on every option change and on exit; migrates `%LocalAppData%\AlloyClient` once |
| account.xml | same folder | username + Base64 password |
| Server addresses | `Settings.cs:35-50` compile-time constants, `TARGET_VPS` define from `-p:DeployTarget=vps` | web build uses page config instead |
| `WAW_LOG` | `Logging/Logging.cs:25` | log level |
| `WAW_TIME`, `WAW_WEATHER` | `DayNight.cs:57`, `Weather.cs:49` | force time / weather locally |
| `WAW_NO_STATIC_BAKE` | `StaticProps.cs:37` | disable baked props |
| `WAW_PERFTEST`, `WAW_PERFTEST_MODE` | `Dev/DevPerfTest.cs:23-36` | perf harness (fake players / enemies / timeline) |
| `WAW_LAUNCHER_ONLY` | `Utils/ClientPlatform.cs:22` | launcher-only login |
| `WAW_LAUNCH_USER`, `WAW_LAUNCH_TOKEN` | `AppEngine/LaunchLogin.cs:22-23` | launcher hand-off |

---

## 18. Debug and performance systems

- `Ui/Components/Elements/DebugStats.cs` (F5, minimap FPS tab), `Game/FrameStats.cs`, `Game/PerfCounters.cs`, `Game/PerfSections.cs` (per-section timers), `WaW.Engine/FrameTiming.cs` (work / swap / sleep / GPU ms), `GpuStats`.
- `Dev/DevPerfTest.cs` (env driven), `Logging/PacketLogger.cs` (`Settings.PacketLogging`), `Game/SunDebug.cs` (`/sundebug` client-only chat command).
- Performance systems: `FixedStepper` cap, culling, chunk meshes, static-prop baking, `GraphicsProfile.Fast` instancing, `ObjectPools.Projectiles`, pooled packets, particle generator cap 300, notification cap, `Map.Enemies` subset for hit tests.

---

## 19. Classification table

| Folder / class | Classification | Notes |
|---|---|---|
| `WaW.Engine/GameWindow.cs`, `FrameTiming.cs`, `GameTime.cs` | Infrastructure | replaced by Unity player loop |
| `WaW.Engine/Graphics/*` | Presentation-rendering | GL wrappers; not ported |
| `WaW.ShaderSourceGen` | Infrastructure (build) | not ported |
| `WaW.ContentBuilder`, `WaW.ContentReader`, `WaW.Common/AtlasConfig`, `Structs/AtlasStructs` | Infrastructure (assets) | replaced by Unity import/atlas |
| `WaW.Audio` | Infrastructure / Presentation | replaced by AudioSource/Mixer |
| `WaW.UiLib` | UI | replaced by UI Toolkit/uGUI |
| `WaWClient/Main.cs`, `Program.cs` | Infrastructure | bootstrap |
| `Display/*` (Screen/Overlay/Dialog/Tooltip managers) | UI | |
| `Screens/*` (title, book, creation, portal, death, loader) | UI | `TitleBattle` is cosmetic simulation |
| `Loading/*` | Infrastructure / UI | load plans, world-load milestones |
| `Networking/Client.cs`, `SocketSendState`, `SocketReceiveState`, `SpanReader/Writer`, `NetworkParsing`, `Packets/**` | Networking | `Packets/*.Handle()` contain gameplay application logic |
| `AppEngine/*` | Networking (HTTP) | |
| `Data/*` | Networking DTOs / client state | |
| `Core/Settings.cs` | Infrastructure | |
| `Core/MinimapIcon.cs`, `Core/Audio.cs` | Presentation | |
| `Game/Map.cs` (`Map`, `TileMap`) | Gameplay logic (world model) + Presentation (Draw) | mixed |
| `Game/MapTile.cs`, `TileBuilder.cs`, `GroundMosaic.cs` | Gameplay (walkability) + Presentation (blending) | mixed |
| `Game/Objects/Entity.cs` | Gameplay (stats, position, interpolation) + Presentation (animation) | mixed |
| `Game/Objects/Player.cs` | Gameplay logic (movement, collision, shooting, speed formulas) + camera control | mixed |
| `Game/Objects/Projectile.cs`, `ProjectilePaths/*`, `Util/EntityUtils.cs` | Gameplay logic | must match server |
| `Game/ConditionEffect.cs` | Gameplay logic | |
| `Game/PartyData.cs` | Gameplay / UI | |
| `Game/Camera.cs`, `CullRules.cs` | Presentation-rendering | |
| `Game/DayNight.cs`, `Weather.cs`, `Shelter.cs` | Presentation (deterministic sim) | time model is a shared rule if the server ever uses it |
| `Game/Lights.cs`, `SightMap.cs`, `Ambience.cs`, `FireGlow.cs`, `Bloom.cs`, `StaticProps.cs`, `SunDebug.cs` | Presentation-rendering | `SightMap` also has visibility meaning |
| `Game/WeatherSound.cs`, `Game/Music/*` | Presentation (audio) | |
| `Game/FixedStepper.cs` | Infrastructure | |
| `Game/PerfCounters.cs`, `PerfSections.cs`, `FrameStats.cs`, `Dev/*`, `Logging/*` | Infrastructure (debug) | |
| `Game/Components/UserInput.cs` | Infrastructure (input) + Gameplay (escape/interact rules) | |
| `Game/Components/Hud/**`, `Options/**`, `Admin/**`, `BugBoard/**`, `Jukebox/**`, `News/**`, `Roles/**`, `WorldLoadCover.cs`, `MinimapTexture.cs` | UI | `ItemTile` holds slot-fit rules |
| `ParticleEffects/*` | Presentation-rendering | |
| `Rendering/**` | Presentation-rendering | |
| `Ui/**` | UI | |
| `Assets/AssetParser.cs`, `Assets/XmlStructs/*`, `Assets/Libraries/*` | Gameplay data (definitions) + asset binding | |
| `Assets/ModelData*.cs` | Presentation | |
| `Utils/*` | Infrastructure | `Texture.cs` = animation frame choice (Presentation) |
| `Shared/Common.Protocol/*` | Networking / shared gameplay rules | |
| `Tests/WaWClient.Tests` | Infrastructure (tests) | |
| `WebClient/**` | Infrastructure (platform port) | retire |

---

## 20. WebClient (WebAssembly + WebGL2)

- **What it shares**: everything. `WebClient/web/web.csproj:59-67` compiles WaW.Common, WaW.ContentReader, WaW.Engine (minus `GameWindow.cs`, `CustomLogger.cs`, `StorageBuffer.cs`), WaW.UiLib, WaWClient (minus `Program.cs`), Common.Protocol into one assembly `WarriorsWeb`. Same game logic, UI, protocol, content.
- **Platform-specific (shims, `WebClient/web/shim/`)**: `GL.cs` + `wwwroot/gl.js` (OpenTK GL -> WebGL2), `Platform.cs` (window/input events), `GameWindow.cs` (requestAnimationFrame loop, frame delta clamped to 250 ms, line 47), `StorageBuffer.cs`/`UniformBuffer.cs` (data textures), `Audio.cs` + `wwwroot/audio.js` (Web Audio; **loops silent** - no rain/thunder audio on web), `WebClient.cs` (WebSocket replacement of `Networking/Client.cs` - a hand-maintained duplicate), `ReFuel.cs` (managed PNG decode), `ClientPlatform.cs`, `WebLogger.cs`, `Enums.g.cs` (1722 lines generated). `web/WebHost.cs` = entry point, JS bridge, localStorage persistence, api/game URL from page config.
- **Exists only because of WASM/WebGL**: the shims above; `web/patched/*` text-patched copies of 6 desktop files + 3 full replacements (`tools/patch_sources.py`, `tools/patch_rules_more.py`: Logging, IndexBuffer, AppEngineClient, Settings timeout 120 s, Shader UBO binding, WorldPosData; replaced Client.cs, UniformBuffer.cs, ClientPlatform.cs); `web/shaders/*` GLSL ES 3.00 ports (`tools/port_shaders.py`) and `tools/shadercheck.py` (headless Chrome compile check); WebSocket-to-TCP bridges (`tools/ws_bridge.py`, `vps/ws_bridge.py`, `vps/setup_web.sh` nginx + websockify + TLS); `vps/patch_cache.py`.
- **Build infra that can be retired**: `tools/build_web.py`, `patch_sources.py`, `patch_rules_more.py`, `port_shaders.py`, `shadercheck.py`, `enumgen/`, `errs.py`, `rebuild.sh`, `serve.py`, `webtest.py` + `acts_*.json`, `ws_bridge.py`, `vps/*`, the whole `web/` project and its `.generated/` output. None of it has a Unity equivalent.
- **Does a Unity WebGL build change its role?** Yes - it makes the hand-built shim layer obsolete. A Unity WebGL build compiles the same Unity client with IL2CPP; what remains needed is only a **WebSocket transport** (browsers cannot open TCP) and a server-side WebSocket endpoint or bridge. The current design pattern (same-origin `/api`, `/game` WebSocket bridge to unchanged TCP game port) is the part worth keeping as a deployment idea.

---

## 21. Client-side gameplay rules the new server must own or the Unity client must replicate

| # | Rule | Reference | Owner in new architecture |
|---|---|---|---|
| R1 | Move speed = `0.004 + speed/75 * (0.0096 - 0.004)` tiles/ms; `Slowed` -> 0.004; `Speedy`/`NinjaSpeedy` x1.5; times ground `Speed` multiplier; `FocusedSpeed` 15 when Focused. **Code uses integer division `speed / 75`** (both ints), so any Speed < 75 gives the minimum speed | `Player.cs:22-25, 691-709` | Server authoritative (validate); client predicts with the SAME formula. Decide the intended formula (float vs int) |
| R2 | Sinking ground: sink level +1 per tick to 18; multiplier `0.1 + (1 - sink/18) * (groundSpeed - 0.1)` | `Player.cs:756-766` | Shared rule |
| R3 | Sliding (ice) ground `SlideAmount` | `Player.cs:216-243` | Shared rule |
| R4 | Collision: walkable tile (`!NoWalk` and no `OccupySquare` occupant), half-tile border sliding, neighbour `FullOccupy`/void blocks the near half; max sub-step 0.4 tiles | `Player.cs:508-676`, `MapTile.cs:126-128` | Shared rule (server `MovementRules.CanEnter` exists) |
| R5 | `Confused` swaps/negates input axes and rotation | `Player.cs:683-688` | Shared rule |
| R6 | Movement direction is relative to the camera angle | `Player.cs:211-235` | Client input mapping only (server sees positions) |
| R7 | Attack frequency `0.0015 + DEX/75 * 0.0065` per ms, `Dazed` min, `Berserk` x1.25, period x 1/RateOfFire; `Stunned`/`Paused` cannot shoot | `Player.cs:26-27, 437-448, 465-479` | Server authoritative rate check; client mirrors for feel |
| R8 | Multi-shot spread: `NumProjectiles` evenly over `ArcGap` degrees centred on aim | `Player.cs:490-494` | Server should spawn from one shoot command |
| R9 | Bullet numbering per shooter 0..1999 wrapping, one number per PlayerShoot in order (refused ones too) | `Player.cs:711-715`, `Projectile.cs:79-80`, CLAUDE.md COMBAT | Server must own or follow exactly |
| R10 | Tutorial ("Initiation") empty weapon slot fires the class's starting weapon | `Player.cs:452-456` | Server rule |
| R11 | Projectile paths (line, wavy, amplitude, circle, boomerang, accelerate, decelerate, change speed, combined) by elapsed ms; lifetime | `Game/Objects/ProjectilePaths/*` | Shared deterministic math (C++ and C#) |
| R12 | Projectile vs walls: stops on void/0xFF, `EnemyOccupySquare`, `OccupySquare` unless `PassesCover` | `Projectile.cs:226-244` | Server authoritative |
| R13 | Hit test: swept circle radius 0.5; player bullets vs enemies; enemy bullets vs local player only; multi-hit once per target | `Projectile.cs:76, 250-304`, `EntityUtils.cs:78-93` | Recommend server authoritative hit detection (today client-reported, server validated) |
| R14 | Escape: not in Nexus, not while reconnecting | `UserInput.cs:178-184` | Server must enforce |
| R15 | Interact radius: squared distance <= 1 (cats: <= 4) | `InteractPanel.cs:59`, `EntityUtils.cs:9-30` | Server enforces use distance |
| R16 | Item slot fitting (`InventoryLayout.SlotFits`), locked backpack | `ItemTile.cs:465, 547`, `Shared/Common.Protocol/InventoryLayout.cs` | Server authoritative; client mirrors for UI |
| R17 | Potion hotkeys use the first matching item by id | `ItemTile.cs:579` | UI convenience |
| R18 | Day/night: 32-minute cycle, `Phase = (UTC minutes % 32)/32`; keys at fixed phases; worlds with sky Nexus/Realm/Campsite | `DayNight.cs:34-35, 62-65` | If gameplay ever depends on it: server owns and broadcasts; else shared deterministic rule |
| R19 | Weather: 7-minute slots, SplitMix-style hash with fixed salts, rain 30 %, storm 35 %, overcast 30 %, 20 s lightning buckets 20 % | `Weather.cs:20-33, 68-115` | Same: deterministic shared rule or server-sent |
| R20 | Quest arrow: server sends quest object id; client points at that entity, shows distance in tiles | `QuestObjId.cs:18-20`, `Hud/QuestArrow.cs` | Server owns selection; client presentation |
| R21 | Remote interpolation: velocity glide over measured interval (30..200 ms), snap > 3 tiles | `Entity.cs:277-305` | Client presentation |
| R22 | Daily bounty = `Shared/Common.Protocol/Bounties.cs` by UTC day number | CLAUDE.md, shared file | Server owns |
| R23 | Unknown object type -> placeholder object 0x017e | `Update.cs:76-79` | Client presentation fallback |
| R24 | Static objects mark their tile occupied (drives collision + minimap) | `Entity.cs:262-268` | Client world model |

## 22. Client-only features that carry gameplay meaning

- Zoom 0.5..5 and `LowerPlayerView` (2.5 tiles ahead) change how much of the world is visible; server sight radius (~20) is the real limit (`SightMap.cs:19` comment).
- Minimap reveals every tile ever received; quest arrow points beyond sight; `NearbyPlayersPanel` lists players.
- Sundial / `Weather.SkyAt` give exact forecasts (pure functions of time).
- `Settings.MovementInterpolation` changes how remote entities are seen (and the off path is broken).
- Client-side hit detection decides which hits are reported at all.
- Client-side collision decides where the player can go (server only validates).
- `/sundebug` chat command (client only, `Game/SunDebug.cs`).
- Admin dashboard and staff tab visibility gated by client rank (`HudView.cs:165`); server re-checks commands.
- Portal "usable" defaults to true when the server sends no stat (`Entity.cs:95-97`).
- `TitleBattle` is a client-only fake battle (no gameplay).

---

## 23. Known issues observed in code (not fixed; audit only)

1. Integer division in move speed (`Player.cs:697`) - see R1.
2. Broken non-interpolated remote path (section 16.2).
3. Password stored Base64 locally (`Settings.cs:387-395, 397-423`) and sent in clear in `Hello` and every HTTP call.
4. `GameScreen.cs:195` comment says fixed steps do "hit tests" - they do not (hit tests are per frame in `Projectile.Update`).
5. `MinimapTexture` allocates 4096x4096 CPU + GPU (`MinimapTexture.cs:16-22`) regardless of map size (realm is 200x200 per CLAUDE.md).
6. Parallel XML parse with `TryAdd` = nondeterministic winner on duplicate ids (`AssetParser.cs:40-72`).
7. Many incoming packets are parsed but ignored (section 15).
8. Static global state everywhere (`Map`, `GlobalData`, `Settings`, `Client`) - not testable in isolation.
9. Dead code: crossed cards / flat stack / 3D model enum / shore coverage (section 4.3).
10. `Ui.frag` and `Particle.frag` still use `dFdx`/`textureGrad` against the project's own driver rules (`Ui.frag:110-152`, `Particle.frag:12`).

---

## 24. Discrepancies (doc says X; code does Y)

| # | Document claim | Code reality |
|---|---|---|
| D1 | `WaW-Client/README.md` "Run Requirements: OpenGL 4.6 support" | Requests GL 3.3 core forward-compatible (`Main.cs:31`, `WaW.Engine/GameWindow.cs:33-39`); all shaders `#version 330` |
| D2 | `Docs/EngineeringAudit.md` 1.2: `Main : GameWindow(GL 4.3)`; Particle/Shadow/Ui.vert `#version 430`; "real minimum is GL 4.3" (also F22) | GL 3.3 (`Main.cs:31`); every shader is 330 (`AdditionalFiles/*:1`) |
| D3 | EngineeringAudit 1.2 / F12: render thread pinned to core 0; `TimeBeginPeriod` never released | Not pinned (`GameWindow.cs:287-291` comment); `TimeEndPeriod(1)` restored (`GameWindow.cs:295`) |
| D4 | EngineeringAudit 1.2: UI `SpriteRender` "ring of 8 buffer sets" | `BufferSetCount = 32` (`WaW.UiLib/Rendering/SpriteRender.cs:23`) |
| D5 | EngineeringAudit 1.2 / 3.1: fixed-update loop runs "projectile hit tests + trails" | Hit tests run per render frame in `Projectile.Update` (`Projectile.cs:179`); `FixedUpdate` only spawns trail sparks (`Projectile.cs:192-202`). `GameScreen.cs:195` comment is also stale |
| D6 | EngineeringAudit F19: entity culling commented out, all entities visible | Culling active via `CullRules` (`Entity.cs:240-250`, `CullRules.cs`) |
| D7 | EngineeringAudit F11: "Damage shown is a client roll" | Damage numbers come from server `Notification`; the client roll (`Player.cs:497`) is unused; `Damage` handler still empty (`Damage.cs:41`) |
| D8 | EngineeringAudit F18: tiles re-expanded on the CPU every frame | Per-chunk GPU meshes rebuilt only when dirty (`Map.cs:121-144`, `Rendering/TileChunkMesh.cs`) |
| D9 | EngineeringAudit 1.2: packet ids are a hand-written enum in `Networking/Packets/PacketIds.cs` + dead `PacketIdOld` | `PacketIds.cs:4` is a global alias to `Shared/Common.Protocol/PacketId.cs` |
| D10 | EngineeringAudit 1.2/1.3 paths `AlloyClient/...`, `Alloy.Engine/...` | Renamed to `WaW-Client/WaWClient`, `WaW.Engine` (CLAUDE.md rename 2026-09-23) |
| D11 | EngineeringAudit 1.3: `Networking/Client.cs`, `UniformBuffer.cs`, `ClientPlatform.cs` are "text-patched copies" | They are fully replaced by hand-written shim files (`WebClient/tools/patch_rules_more.py` `replace_file`); `web/patched.props` removes them |
| D12 | EngineeringAudit 1.3: "No feature is implemented twice" | `WebClient/web/shim/WebClient.cs` is a second implementation of `Networking/Client.cs` that must be mirrored by hand (CLAUDE.md says so); web audio loops are not implemented (`shim/Audio.cs` `StartLoop` returns -1) |
| D13 | CLAUDE.md "CAMERA TILT ... GroundDepthScale (0.5 now)" | `GroundDepthScale = 1f` (`Camera.cs:16`); CLAUDE.md's later "BACK TO A FLAT WORLD" section agrees with code |
| D14 | CLAUDE.md JUMPING section (Space, `Player.TryJump`, packet 77, `JumpRules`) | No jump in client: Special moved to R (`Settings.cs:80-81`), no jump case in `UserInput.cs`; later CLAUDE.md section says jumping removed |
| D15 | CLAUDE.md SHORES: "BANK LIPS (`Game/BankLips.cs`)" | File does not exist; `MapTile.ShoreCoverage` still computed but unused |
| D16 | CLAUDE.md: "SightMap.LineOfSightWorlds must equal ... RatCave, GrimsLair" | Empty array (`SightMap.cs:27`); later CLAUDE.md section agrees |
| D17 | CLAUDE.md says `TypeCrossedCards`/`TypeFlatStack`/3D models removed | Render-side helpers and model enum remain as dead code (`Render.Draw.cs:330, 369`, `Render.Baked.cs`, `ModelData.cs:12+`) |
| D18 | `WebClient/tools/patch_sources.py` docstring: "Run before every web build (build.ps1 does)" | The runner is `tools/build_web.py` (lines ~37-38); no `build.ps1` in `WebClient/` |
| D19 | `WaW-Client/README.md` describes "Alloy Client ... for Realm of the Mad God private servers" | Project is Warriors & Wizards; README is upstream text |
| D20 | EngineeringAudit F3: `ParticleGenerators` unbounded | Capped at 300 (`Map.cs:472-492`) |
