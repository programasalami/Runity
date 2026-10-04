using System;
using System.Linq;
using System.Net;
using System.Threading;
using System.Threading.Tasks;
using UnityEngine;
using Runity.Client;
using Runity.Domain.Content;
using Runity.Presentation.Art;
using Runity.Presentation.Input;
using Runity.Presentation.UI;
using Runity.Presentation.UI.Alloy;
using Runity.Presentation.World;

namespace Runity.Presentation.Boot
{
    /// <summary>The client's single entry point (scene "Game"). It owns the services and switches between the sign-in, character
    /// and play screens. Game state lives in GameSession / ClientWorld (plain C#); this component only connects them to Unity.</summary>
    public sealed class GameBootstrap : MonoBehaviour
    {
        [Tooltip("Optional: the project's own art for the definitions' art keys. Empty = placeholders everywhere.")]
        public ArtCatalog Art;
        [Tooltip("Unlit sprite material (URP 2D Sprite-Unlit-Default); created by Runity > Build Game Scene.")]
        public Material SpriteMaterial;

        private const float PortalRange = 1.8f;  // a little inside the server's 2 tiles

        private ClientSettings _settings;
        private ContentCatalog _content;
        private ApiClient _api;
        private GameSession _session;
        private PlayerControls _controls;
        private Ui _ui;
        private FrontEnd _front;
        private MusicPlayer _music;
        private AlloyHud _hud;
        private DevStats _devStats;
        private DevConsole _console;
        private int _maxFps = 60;  // the options screen will let the player choose 120, 300, ... (never unlimited)
        private WorldView _worldView;
        private AccountInfo _account;
        private int? _pendingLoad;
        private ushort? _pendingCreate;

        /// <summary>For automated play-mode tests.</summary>
        public GameSession Session => _session;
        public WorldView WorldView => _worldView;
        public string LastStatus { get; private set; }

        private void Start()
        {
            WindowTitle.Set("Runity - [F4-CONSOLE, F5-STATS]");
            _settings = ClientSettings.FromEnvironment();
            var font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            _ui = new Ui(font);
            if (SpriteMaterial == null)
            {
                var shader = Shader.Find("Universal Render Pipeline/2D/Sprite-Unlit-Default") ?? Shader.Find("Sprites/Default");
                SpriteMaterial = new Material(shader);
            }

            try
            {
                _content = ContentCatalog.LoadDirectory(ClientSettings.DefinitionsDirectory());
            }
            catch (Exception e)
            {
                Debug.LogError($"[Runity] game definitions could not be loaded: {e.Message}");
                _ui.Label(_ui.Canvas.transform, "The game data could not be loaded:\n" + e.Message, 24, TextAnchor.MiddleCenter);
                enabled = false;
                return;
            }
            try
            {
                SheetAtlas.Shared = new SheetAtlas(ClientSettings.ArtDirectory());
                Debug.Log($"[Runity] art: {SheetAtlas.Shared.EntryCount} sheets indexed");
            }
            catch (Exception e)
            {
                // The game still runs on placeholders without the art folder.
                Debug.LogWarning($"[Runity] art not loaded ({e.Message}): drawing placeholders");
            }

            _api = new ApiClient(_settings.ApiUrl) { Trace = m => Debug.Log("[Runity] " + m) };
            _music = gameObject.AddComponent<MusicPlayer>();
            _music.Play("Sound/Music/sorc.ogg");
            _controls = new PlayerControls();

            var worldGo = new GameObject("World");
            _worldView = worldGo.AddComponent<WorldView>();
            _worldView.Init(Camera.main != null ? Camera.main : new GameObject("Main Camera", typeof(Camera)).GetComponent<Camera>(),
                Art, SpriteMaterial, font, _content);

            _front = new FrontEnd(_ui.Canvas, _content)
            {
                Login = (n, p) => SignInAsync(n, p, register: false),
                Register = (n, p) => SignInAsync(n, p, register: true),
                Logout = () => Run(SignOutAsync()),
                PlayCharacter = id => Run(PlayAsync(id, null)),
                CreateCharacter = cls => Run(PlayAsync(null, cls)),
                Quit = Application.Quit,
                ToggleMusic = () => _music.Toggle(),
                MusicOn = () => MusicPlayer.Enabled,
            };
            _front.Servers.Add(new ServerEntry { Name = "Localhost", Port = 2050, Players = 0, MaxPlayers = 100 });
            _hud = new AlloyHud(_ui.Canvas, _content);
            _hud.ChatSubmitted += text => _session?.SendChat(text);
            _devStats = new DevStats(_ui.Canvas);
            _console = new DevConsole(_ui.Canvas, _devStats, _worldView.CameraRig, () => _maxFps, fps => _maxFps = fps);

            ShowOnly(front: true);
            if (!string.IsNullOrEmpty(_settings.LaunchToken))
            {
                _api.UseToken(_settings.LaunchToken);
                Run(StartupAsync(showCharacters: true));
            }
            else if (!string.IsNullOrEmpty(_settings.AutoLoginName))
            {
                Run(AutoPlayAsync(_settings.AutoLoginName, _settings.AutoLoginPassword));
            }
            else
            {
                Run(StartupAsync(showCharacters: false));
            }
        }

        /// <summary>The original LoadingScreen: "Loading..." for at least 2 s while the service is contacted, then the title screen
        /// (or the retry dialog there when the service is unreachable).</summary>
        private async Task StartupAsync(bool showCharacters)
        {
            _front.ShowLoading();
            var minimum = Task.Delay(2000);
            try
            {
                await _api.GetVersionAsync();
                if (showCharacters) await RefreshAccountAsync();
            }
            catch (Exception e)
            {
                _front.FailureMessage = e is TaskCanceledException ? "Server timed out!" : "Server offline!";
            }
            await minimum;
            if (showCharacters && _front.Account != null) _front.ShowCharacters();
            else _front.ShowTitle();
        }

        private async Task RefreshAccountAsync()
        {
            _account = await _api.GetAccountAsync();
            _front.SetAccount(_account);
        }

        /// <summary>Starts a UI flow without awaiting it. Unexpected failures are logged and shown, never silently dropped.</summary>
        private void Run(Task task)
        {
            task.ContinueWith(t =>
            {
                var e = t.Exception?.GetBaseException();
                Debug.LogException(e);
                Status("Something went wrong: " + e?.Message);
            }, CancellationToken.None, TaskContinuationOptions.OnlyOnFaulted, TaskScheduler.FromCurrentSynchronizationContext());
        }

        private void ShowOnly(bool front = false, bool hud = false)
        {
            _front.Show(front);
            _hud.Show(hud);
        }

        private void Status(string text)
        {
            LastStatus = text;
            if (!string.IsNullOrEmpty(text)) Debug.Log("[Runity] " + text);
        }

        /// <summary>The login / register overlays' request: null on success, else the error dialog's message.</summary>
        private async Task<string> SignInAsync(string name, string password, bool register)
        {
            try
            {
                if (register) await _api.RegisterAsync(name, password);
                await _api.LoginAsync(name, password);
                await RefreshAccountAsync();
                return null;
            }
            catch (ApiException e)
            {
                return e.Status == HttpStatusCode.Unauthorized ? "Invalid account credentials." : e.Message;
            }
            catch (Exception)
            {
                return "Failed to contact server.";
            }
        }

        private async Task ShowCharactersAsync()
        {
            try
            {
                await RefreshAccountAsync();
                ShowOnly(front: true);
                _front.ShowCharacters();
            }
            catch (Exception e)
            {
                Debug.LogWarning("[Runity] account: " + e.Message);
                ShowOnly(front: true);
                _front.SetAccount(null);
                _front.ShowTitle();
            }
        }

        private async Task SignOutAsync()
        {
            try
            {
                await _api.LogoutAsync();
            }
            catch (ApiException)
            {
                // The session ends locally either way.
            }
            _front.SetAccount(null);
            Status("");
        }

        /// <summary>Development / test automation (RUNITY_AUTOLOGIN_NAME / _PASSWORD): sign in (registering if needed), then play the first
        /// character or create a Wizard.</summary>
        private async Task AutoPlayAsync(string name, string password)
        {
            Status($"Automatic sign-in as {name}...");
            try
            {
                try
                {
                    await _api.LoginAsync(name, password);
                }
                catch (ApiException e) when (e.Status == HttpStatusCode.Unauthorized)
                {
                    Status($"Automatic sign-in: registering {name}...");
                    await _api.RegisterAsync(name, password);
                    await _api.LoginAsync(name, password);
                }
                Status("Automatic sign-in: signed in, reading the account...");
                await RefreshAccountAsync();
                var first = _account.Characters.FirstOrDefault();
                var wizard = _content.Classes.FirstOrDefault(c => c.Id == "Wizard") ?? _content.Classes.First();
                await PlayAsync(first?.CharacterId, first == null ? wizard.Type : (ushort?)null);
            }
            catch (ApiException e)
            {
                Status("Automatic sign-in failed: " + e.Message);
            }
        }

        private async Task PlayAsync(int? characterId, ushort? create)
        {
            Status("Joining the game...");
            JoinInfo join;
            try
            {
                join = await _api.JoinAsync();
            }
            catch (ApiException e)
            {
                Status(e.Message);
                _front.ShowDialog("Error", e.Message, "Ok", null);
                return;
            }
            _session?.Dispose();
            _session = new GameSession(_content, _settings.BuildVersion);
            _session.PhaseChanged += OnPhaseChanged;
            _worldView.Attach(_session.World);
            _hud.Attach(_session);
            _pendingLoad = characterId;
            _pendingCreate = create;
            ShowOnly(hud: true);
            _hud.SetOverlay("Connecting...");
            await _session.StartAsync(join.GameHost, join.GamePort, join.Ticket);
        }

        private void OnPhaseChanged(SessionPhase phase)
        {
            switch (phase)
            {
                case SessionPhase.CharacterSelect:
                    if (_pendingCreate is { } c) _session.CreateCharacter(c);
                    else if (_pendingLoad is { } id) _session.LoadCharacter(id);
                    _pendingCreate = null;
                    _pendingLoad = null;
                    _hud.SetOverlay("Entering the world...");
                    break;
                case SessionPhase.InWorld:
                    _hud.SetOverlay(null);
                    Status("");
                    break;
                case SessionPhase.Closed:
                    var reason = _session.EndReason ?? "Disconnected.";
                    Status(reason);
                    // The original has no death or disconnect screen: death fades to the title, anything else to the characters.
                    _diedLast = _session.World.Death != null;
                    Invoke(nameof(BackToCharacters), _diedLast ? 1f : 0.2f);
                    break;
            }
        }

        private bool _diedLast;

        private void BackToCharacters()
        {
            _worldView.Detach();
            _session?.Dispose();
            _session = null;
            _hud.SetOverlay(null);
            if (_diedLast)
            {
                _diedLast = false;
                ShowOnly(front: true);
                Run(RefreshAccountAsync());
                _front.ShowTitle();
                return;
            }
            Run(ShowCharactersAsync());
        }

        private void LeaveGame()
        {
            CancelInvoke(nameof(BackToCharacters));
            BackToCharacters();
        }

        private void Update()
        {
            // Never uncapped: the frame cap, or 30 in the background. VSync is off in every quality level, so this is the only limit.
            Application.targetFrameRate = Application.isFocused ? _maxFps : 30;
            var consoleWasOpen = _console.IsOpen;  // the frame Esc closes the console must not also leave the world
            _devStats.Tick(_session);
            _console.Tick(_session);
            if (_session == null) return;
            var typing = _hud.ChatFocused || consoleWasOpen || _console.IsOpen;
            if (_session.Phase == SessionPhase.InWorld && !typing)
            {
                if (_controls.ChatPressed) _hud.FocusChat();
                if (_controls.ResetCameraPressed) _worldView.CameraRig.ResetAngle();
                _worldView.CameraRig.Turn(_controls.Turn, Time.unscaledDeltaTime);
            }
            var move = typing ? Vector2.zero : _controls.Move;
            var (dx, dy) = InputMapping.ToWorldDirection(move.x, move.y, _worldView.CameraRig.AngleRadians);

            var firing = false;
            var aim = 0f;
            if (_session.Phase == SessionPhase.InWorld && !typing)
            {
                var overUi = UnityEngine.EventSystems.EventSystem.current != null &&
                             UnityEngine.EventSystems.EventSystem.current.IsPointerOverGameObject();
                firing = _controls.FireHeld && !overUi;
                var target = _worldView.ScreenToWorld(_controls.AimScreenPosition);
                var me = _session.World.Predictor.Position;
                aim = Mathf.Atan2(target.Y - me.Y, target.X - me.X);

                var portal = _session.NearestPortal(PortalRange);
                if (portal != null && _controls.InteractPressed) _session.UsePortal(portal.Id);
                if (_controls.NexusPressed) _session.ReturnToNexus();
                // The original's camera zoom: Shift + mouse wheel (0.5x .. 5x).
                var mouse = UnityEngine.InputSystem.Mouse.current;
                var keys = UnityEngine.InputSystem.Keyboard.current;
                if (mouse != null && keys != null && keys.shiftKey.isPressed && !overUi)
                {
                    var wheel = mouse.scroll.ReadValue().y;
                    if (Mathf.Abs(wheel) > 0.01f) _worldView.CameraRig.ZoomBy(Mathf.Sign(wheel));
                }
                // The original's options "home": leave the world (Escape).
                if (keys != null && keys.escapeKey.wasPressedThisFrame)
                {
                    LeaveGame();
                    return;  // the session is gone
                }
            }
            _session.Update(Time.unscaledDeltaTime * 1000f, dx, dy, firing, aim);
        }

        private void LateUpdate()
        {
            if (_session == null) return;
            _worldView.Render();
            _worldView.CameraRig.HudWidthPixels = AlloyHud.Width * _ui.Canvas.scaleFactor;
            _hud.Refresh(_session.World, _session);
        }

        private void OnDestroy()
        {
            _session?.Dispose();
            _api?.Dispose();
            _controls?.Dispose();
            _console?.Dispose();
        }
    }
}
