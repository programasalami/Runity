using System;
using System.Collections.Generic;
using System.Linq;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.UI;
using Runity.Client;
using Runity.Presentation.UI.Alloy;
using Runity.Presentation.World;

namespace Runity.Presentation.UI
{
    /// <summary>The in-client developer console, toggled with F4: commands that change the client live (frame cap, VSync,
    /// quality, resolution, camera) or report on it (specs, network, memory), the client's own log, and the F5 numbers on one live
    /// line on top. It drops down over the top of the play area, left of the HUD, so the game and the chat stay in view.</summary>
    public sealed class DevConsole : IDisposable
    {
        private const float Height = 300f;
        private const float Width = 1280f - AlloyHud.Width;
        private const int FontSize = 13;
        private const int VisibleLines = 14;
        private const int KeptLines = 300;
        private const float StripRefreshSeconds = 0.25f;

        private static readonly Color Background = new Color(0.03f, 0.03f, 0.05f, 0.86f);
        private static readonly Color FieldBackground = new Color(1f, 1f, 1f, 0.07f);
        private static readonly Color Divider = new Color(1f, 1f, 1f, 0.12f);

        private sealed class Command
        {
            public string Usage;
            public string Help;
            public Action<string[]> Run;
        }

        private readonly RectTransform _panel;
        private readonly Text _strip;
        private readonly Text _log;
        private readonly InputField _input;
        private readonly DevStats _stats;
        private readonly CameraRig _camera;
        private readonly Func<int> _getMaxFps;
        private readonly Action<int> _setMaxFps;
        private readonly Dictionary<string, Command> _commands = new Dictionary<string, Command>();
        private readonly List<string> _lines = new List<string>();
        private readonly List<string> _history = new List<string>();
        private GameSession _session;
        private int _historyIndex;
        private bool _logDirty;
        private float _sinceStrip = StripRefreshSeconds;

        public DevConsole(Canvas canvas, DevStats stats, CameraRig camera, Func<int> getMaxFps, Action<int> setMaxFps)
        {
            _stats = stats;
            _camera = camera;
            _getMaxFps = getMaxFps;
            _setMaxFps = setMaxFps;

            var image = AlloyUi.Rect(canvas.transform, "Developer Console", Background);
            _panel = image.rectTransform.At(0, 0, Width, Height, AlloyUi.LeftTop);
            var layer = image.gameObject.AddComponent<Canvas>();
            layer.overrideSorting = true;
            layer.sortingOrder = short.MaxValue;
            image.gameObject.AddComponent<GraphicRaycaster>();  // clicks on the console do not reach the game

            _strip = AlloyUi.Text(_panel, "", 12, FontType.Bold, AlloyUi.Hover).Place(10, 7, AlloyUi.LeftTop);
            AlloyUi.Rect(_panel, "Divider", Divider).rectTransform.At(0, 26, Width, 1, AlloyUi.LeftTop);

            // The log grows upwards from just above the input line; older lines are clipped at the top.
            var logArea = AlloyUi.Node(_panel, "Log").At(10, 30, Width - 20, Height - 66, AlloyUi.LeftTop);
            logArea.gameObject.AddComponent<RectMask2D>();
            _log = AlloyUi.Text(logArea, "", FontSize, FontType.Normal, AlloyUi.White, 0, Width - 20);
            _log.supportRichText = true;
            _log.alignment = TextAnchor.LowerLeft;
            var logRect = _log.rectTransform;
            logRect.anchorMin = logRect.anchorMax = Vector2.zero;
            logRect.pivot = Vector2.zero;
            logRect.anchoredPosition = Vector2.zero;

            var box = AlloyUi.Rect(_panel, "Input", FieldBackground);
            box.rectTransform.At(8, Height - 32, Width - 16, 24, AlloyUi.LeftTop);
            _input = box.gameObject.AddComponent<InputField>();
            _input.textComponent = FieldText(box.transform, "", AlloyUi.White);
            var placeholder = FieldText(box.transform, "type a command   ·   help lists them   ·   F4 or Esc closes", AlloyUi.Muted);
            placeholder.fontStyle = FontStyle.Italic;
            _input.placeholder = placeholder;
            _input.lineType = InputField.LineType.SingleLine;
            _input.caretColor = Color.white;
            _input.customCaretColor = true;
            _input.onSubmit.AddListener(Submit);

            RegisterCommands();
            Application.logMessageReceived += OnLog;
            Print("<color=#FFDC85>Runity developer console</color>   help lists the commands");
            _panel.gameObject.SetActive(false);
        }

        public bool IsOpen => _panel.gameObject.activeSelf;

        /// <summary>True while the console is open: it has the keyboard (the front end's Tab / Enter keys stand aside).</summary>
        public static bool HasKeyboard { get; private set; }

        /// <summary>Call once per frame, on every screen; `session` is null outside the game.</summary>
        public void Tick(GameSession session)
        {
            _session = session;
            var keys = UnityEngine.InputSystem.Keyboard.current;
            if (keys == null) return;
            if (keys.f4Key.wasPressedThisFrame && !OtherFieldFocused())
            {
                if (IsOpen) Close();
                else Open();
            }
            if (!IsOpen) return;
            if (keys.escapeKey.wasPressedThisFrame)
            {
                Close();
                return;
            }
            if (keys.upArrowKey.wasPressedThisFrame) Recall(-1);
            else if (keys.downArrowKey.wasPressedThisFrame) Recall(1);
            if (!_input.isFocused) _input.ActivateInputField();

            _sinceStrip += Time.unscaledDeltaTime;
            if (_sinceStrip >= StripRefreshSeconds)
            {
                _sinceStrip = 0f;
                _strip.text = _stats.Summary(session);
            }
            if (_logDirty)
            {
                _log.text = string.Join("\n", _lines.Skip(Math.Max(0, _lines.Count - VisibleLines)));
                _logDirty = false;
            }
        }

        public void Dispose() => Application.logMessageReceived -= OnLog;

        private void Open()
        {
            HasKeyboard = true;
            _panel.gameObject.SetActive(true);
            _stats.Suppressed = true;
            _sinceStrip = StripRefreshSeconds;
            _logDirty = true;
            _input.text = "";
            _input.ActivateInputField();
        }

        private void Close()
        {
            HasKeyboard = false;
            _input.DeactivateInputField();
            _panel.gameObject.SetActive(false);
            _stats.Suppressed = false;
            if (EventSystem.current != null) EventSystem.current.SetSelectedGameObject(null);
        }

        private bool OtherFieldFocused()
        {
            var selected = EventSystem.current != null ? EventSystem.current.currentSelectedGameObject : null;
            return selected != null && selected != _input.gameObject && selected.TryGetComponent(out InputField field) && field.isFocused;
        }

        private void Submit(string text)
        {
            Execute(text);
            _input.text = "";
            _input.ActivateInputField();
        }

        private void Recall(int direction)
        {
            if (_history.Count == 0) return;
            _historyIndex = Mathf.Clamp(_historyIndex + direction, 0, _history.Count);
            _input.text = _historyIndex < _history.Count ? _history[_historyIndex] : "";
            _input.caretPosition = _input.text.Length;
        }

        private void Execute(string text)
        {
            text = text.Trim();
            if (text.Length == 0) return;
            _history.Add(text);
            _historyIndex = _history.Count;
            Print($"<color=#8A8F9E>› {Esc(text)}</color>");
            var parts = text.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
            if (!_commands.TryGetValue(parts[0].ToLowerInvariant(), out var command))
            {
                Error($"unknown command '{parts[0]}' (help lists them)");
                return;
            }
            try
            {
                command.Run(parts.Skip(1).ToArray());
            }
            catch (ArgumentException e)
            {
                Error(e.Message);
            }
        }

        private void RegisterCommands()
        {
            Add("help", "help", "lists the commands", _ =>
            {
                foreach (var c in _commands.Values) Print($"<color=#FFDC85>{c.Usage}</color>   {c.Help}");
            });
            Add("clear", "clear", "empties the console", _ =>
            {
                _lines.Clear();
                _logDirty = true;
            });
            Add("fps", "fps [10-1000]", "shows or sets the frame cap until the game closes (never unlimited)", a =>
            {
                if (a.Length > 0) _setMaxFps(Int(a[0], 10, 1000));
                Print($"frame cap {_getMaxFps()} FPS, running at {_stats.Fps:0}" +
                      (QualitySettings.vSyncCount > 0 ? "   (VSync is on: the monitor sets the pace)" : ""));
            });
            Add("vsync", "vsync [on|off]", "shows or switches VSync", a =>
            {
                if (a.Length > 0) QualitySettings.vSyncCount = OnOff(a[0]) ? 1 : 0;
                Print(QualitySettings.vSyncCount > 0 ? "VSync on (the frame cap is ignored while it is on)" : "VSync off");
            });
            Add("quality", "quality [level]", "lists or sets the quality level (name or number)", a =>
            {
                var names = QualitySettings.names;
                if (a.Length > 0)
                {
                    var level = int.TryParse(a[0], out var n) ? n : Array.FindIndex(names, x => x.Equals(a[0], StringComparison.OrdinalIgnoreCase));
                    if (level < 0 || level >= names.Length) throw new ArgumentException($"no quality level '{a[0]}'");
                    QualitySettings.SetQualityLevel(level, true);
                }
                var current = QualitySettings.GetQualityLevel();
                Print(string.Join("    ", names.Select((x, i) => i == current ? $"<color=#FFDC85>[{i}] {x}</color>" : $"[{i}] {x}")));
            });
            Add("res", "res [width height]", "shows or sets the window size", a =>
            {
                if (a.Length == 1) throw new ArgumentException("usage: res <width> <height>");
                if (a.Length >= 2)
                {
                    int w = Int(a[0], 320, 7680), h = Int(a[1], 240, 4320);
                    Screen.SetResolution(w, h, Screen.fullScreenMode);
                    Print($"window size set to {w}x{h}");
                    return;
                }
                var d = Screen.currentResolution;
                Print($"window {Screen.width}x{Screen.height} ({Screen.fullScreenMode})   display {d.width}x{d.height} @ {d.refreshRateRatio.value:0.##} Hz");
            });
            Add("fullscreen", "fullscreen [on|off]", "shows or switches full screen", a =>
            {
                if (a.Length > 0) Screen.fullScreenMode = OnOff(a[0]) ? FullScreenMode.FullScreenWindow : FullScreenMode.Windowed;
                Print(a.Length > 0 ? $"switching to {(OnOff(a[0]) ? "full screen" : "a window")}" : $"display mode {Screen.fullScreenMode}");
            });
            Add("zoom", "zoom [0.5-5]", "shows or sets the camera zoom", a =>
            {
                if (a.Length > 0) _camera.Zoom = Float(a[0], 0.5f, 5f);
                Print($"camera zoom {_camera.Zoom:0.##}x");
            });
            Add("stats", "stats", "turns the F5 stats box on or off (it is hidden while the console is open)", _ =>
            {
                _stats.Shown = !_stats.Shown;
                Print($"stats box {(_stats.Shown ? "on" : "off")}");
            });
            Add("net", "net", "connection and prediction details", _ => Network());
            Add("specs", "specs", "this machine and the game build", _ => Specs());
            Add("gc", "gc", "runs a full garbage collection and reports what it freed", _ =>
            {
                var before = GC.GetTotalMemory(false);
                GC.Collect();
                GC.WaitForPendingFinalizers();
                var after = GC.GetTotalMemory(true);
                Print($"managed memory {DevStats.Mb(before)} → {DevStats.Mb(after)}   (freed {DevStats.Mb(Math.Max(0, before - after))}, " +
                      $"{GC.CollectionCount(0)} collections so far)");
            });
            Add("quit", "quit", "closes the game", _ => Application.Quit());
        }

        private void Network()
        {
            var s = _session;
            if (s == null)
            {
                Print("not connected");
                return;
            }
            Print($"session {s.Phase}" + (s.EndReason != null ? $"   ended: {Esc(s.EndReason)}" : ""));
            Print($"ping {s.RoundTripMs:0} ms   in {DevStats.Kb(_stats.InPerSecond)}/s ({DevStats.Mb(s.Connection.BytesReceived)} total)   " +
                  $"out {DevStats.Kb(_stats.OutPerSecond)}/s ({DevStats.Mb(s.Connection.BytesSent)} total)");
            var w = s.World;
            if (w.Session != null) Print($"account {Esc(w.Session.AccountName)} (#{w.Session.AccountId})");
            if (!w.InWorld) return;
            Print($"world {w.Info.Name}   server tick {w.LastServerTick}   character #{w.LocalCharacterId}   {w.Entities.Count} entities in view");
            Print($"prediction: {w.Predictor.PendingCount} inputs waiting for the server, {w.Predictor.Corrections} corrections " +
                  $"(last {w.Predictor.LastCorrection:0.000} tiles)");
        }

        private void Specs()
        {
            var d = Screen.currentResolution;
            Print($"<color=#B3B3B3>OS</color>   {SystemInfo.operatingSystem}");
            Print($"<color=#B3B3B3>CPU</color>   {SystemInfo.processorType}   {SystemInfo.processorCount} threads   {SystemInfo.processorFrequency} MHz");
            Print($"<color=#B3B3B3>RAM</color>   {SystemInfo.systemMemorySize} MB");
            Print($"<color=#B3B3B3>GPU</color>   {SystemInfo.graphicsDeviceName}   {SystemInfo.graphicsMemorySize} MB   {SystemInfo.graphicsDeviceVersion}");
            Print($"<color=#B3B3B3>Display</color>   {d.width}x{d.height} @ {d.refreshRateRatio.value:0.##} Hz   window {Screen.width}x{Screen.height} {Screen.fullScreenMode}");
            Print($"<color=#B3B3B3>Build</color>   Runity {Application.version}   Unity {Application.unityVersion}   " +
                  $"quality {QualitySettings.names[QualitySettings.GetQualityLevel()]}");
        }

        private void OnLog(string message, string stackTrace, LogType type)
        {
            var color = type == LogType.Warning ? "#FFD54A" : type == LogType.Log ? "#B3B3B3" : "#FF6B6B";
            Print($"<color={color}>{Esc(message)}</color>");
        }

        private void Add(string name, string usage, string help, Action<string[]> run) =>
            _commands[name] = new Command { Usage = usage, Help = help, Run = run };

        private void Print(string line)
        {
            _lines.Add(line);
            if (_lines.Count > KeptLines) _lines.RemoveRange(0, _lines.Count - KeptLines);
            _logDirty = true;
        }

        private void Error(string message) => Print($"<color=#FF6B6B>{Esc(message)}</color>");

        private static Text FieldText(Transform parent, string text, Color color)
        {
            var t = AlloyUi.Text(parent, text, FontSize, FontType.Normal, color);
            UnityEngine.Object.Destroy(t.GetComponent<ContentSizeFitter>());
            t.alignment = TextAnchor.MiddleLeft;
            t.rectTransform.Fill();
            t.rectTransform.offsetMin = new Vector2(8, 0);
            t.rectTransform.offsetMax = new Vector2(-8, 0);
            return t;
        }

        private static string Esc(string s) => (s ?? "").Replace("<", "‹").Replace(">", "›");

        private static int Int(string s, int min, int max) =>
            int.TryParse(s, out var n) && n >= min && n <= max ? n : throw new ArgumentException($"'{s}' is not a number from {min} to {max}");

        private static float Float(string s, float min, float max) =>
            float.TryParse(s, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var f) && f >= min && f <= max
                ? f
                : throw new ArgumentException($"'{s}' is not a number from {min} to {max}");

        private static bool OnOff(string s)
        {
            switch (s.ToLowerInvariant())
            {
                case "on": case "1": case "true": return true;
                case "off": case "0": case "false": return false;
                default: throw new ArgumentException($"'{s}' is not on or off");
            }
        }
    }
}
