using System;
using System.Text;
using UnityEngine;
using UnityEngine.Profiling;
using UnityEngine.UI;
using Runity.Client;
using Runity.Net;
using Runity.Presentation.UI.Alloy;

namespace Runity.Presentation.UI
{
    /// <summary>The developer overlay in the top-left corner, toggled with F5: frame rate and frame times, memory, network and world
    /// numbers. Measured over quarter-second windows (average and worst frame of the last window), drawn above every other screen and
    /// never in the way of the mouse. The developer console shows the same numbers on one line (Summary).</summary>
    public sealed class DevStats
    {
        private const float RefreshSeconds = 0.25f;
        private const float Width = 340f;
        private const float Padding = 8f;
        private const int FontSize = 12;

        private static readonly Color Background = new Color(0.04f, 0.04f, 0.06f, 0.78f);
        private static readonly Color Accent = AlloyUi.Hover;

        private readonly RectTransform _panel;
        private readonly Text _labels;
        private readonly Text _values;
        private readonly StringBuilder _labelText = new StringBuilder(256);
        private readonly StringBuilder _valueText = new StringBuilder(256);

        private bool _shown;
        private float _windowSeconds;
        private int _windowFrames;
        private float _windowWorstMs;
        private GameConnection _connection;
        private long _lastReceived;
        private long _lastSent;

        public DevStats(Canvas canvas)
        {
            var image = AlloyUi.Rect(canvas.transform, "Developer Stats", Background);
            image.raycastTarget = false;
            _panel = image.rectTransform.At(6, 6, Width, 100, AlloyUi.LeftTop);
            // Its own sorting layer above every screen and overlay the game builds later.
            var layer = image.gameObject.AddComponent<Canvas>();
            layer.overrideSorting = true;
            layer.sortingOrder = short.MaxValue;

            AlloyUi.Text(_panel, $"RUNITY {Application.version}  ·  F5", FontSize, FontType.Bold, Accent).Place(Padding, Padding, AlloyUi.LeftTop);
            _labels = AlloyUi.Text(_panel, "", FontSize, FontType.Normal, AlloyUi.Muted).Place(Padding, Padding + FontSize + 6, AlloyUi.LeftTop);
            _values = AlloyUi.Text(_panel, "", FontSize, FontType.Normal, AlloyUi.White).Place(Padding + 78, Padding + FontSize + 6, AlloyUi.LeftTop);
            _labels.alignment = _values.alignment = TextAnchor.UpperLeft;
            _panel.gameObject.SetActive(false);
        }

        /// <summary>F5's on/off state (also the console's "stats" command).</summary>
        public bool Shown
        {
            get => _shown;
            set
            {
                _shown = value;
                _panel.gameObject.SetActive(_shown && !Suppressed);
            }
        }

        /// <summary>Hidden while the developer console is open (it shows the same numbers); F5's state is kept.</summary>
        public bool Suppressed { get; set; }

        public float Fps { get; private set; }
        public float FrameMs { get; private set; }
        public float WorstFrameMs { get; private set; }
        public float InPerSecond { get; private set; }
        public float OutPerSecond { get; private set; }

        /// <summary>Call once per frame, on every screen; `session` is null outside the game.</summary>
        public void Tick(GameSession session)
        {
            var keys = UnityEngine.InputSystem.Keyboard.current;
            if (keys != null && keys.f5Key.wasPressedThisFrame) _shown = !_shown;
            _panel.gameObject.SetActive(_shown && !Suppressed);

            _windowSeconds += Time.unscaledDeltaTime;
            _windowFrames++;
            _windowWorstMs = Mathf.Max(_windowWorstMs, Time.unscaledDeltaTime * 1000f);
            if (_windowSeconds < RefreshSeconds) return;

            FrameMs = _windowSeconds * 1000f / _windowFrames;
            Fps = 1000f / FrameMs;
            WorstFrameMs = _windowWorstMs;
            SampleNetwork(session?.Connection, _windowSeconds);
            if (_panel.gameObject.activeSelf) Refresh(session);
            _windowSeconds = 0f;
            _windowFrames = 0;
            _windowWorstMs = 0f;
        }

        /// <summary>The headline numbers on one line.</summary>
        public string Summary(GameSession session)
        {
            var line = $"FPS {Fps:0}  ·  {FrameMs:0.0} ms  ·  worst {WorstFrameMs:0.0} ms  ·  cap {Application.targetFrameRate}  ·  " +
                       $"mem {Mb(GC.GetTotalMemory(false))} / {Mb(Profiler.GetTotalAllocatedMemoryLong())}";
            if (session == null) return line + "  ·  offline";
            line += $"  ·  ping {(session.RoundTripMs > 0 ? $"{session.RoundTripMs:0} ms" : "-")}  ·  in {Kb(InPerSecond)}/s  ·  out {Kb(OutPerSecond)}/s";
            return session.World.InWorld ? line + $"  ·  {session.World.Entities.Count} entities" : line;
        }

        private void SampleNetwork(GameConnection connection, float seconds)
        {
            if (connection != _connection)
            {
                // A new connection starts its byte counters at zero.
                _connection = connection;
                _lastReceived = _lastSent = 0;
            }
            if (connection == null)
            {
                InPerSecond = OutPerSecond = 0f;
                return;
            }
            var received = connection.BytesReceived;
            var sent = connection.BytesSent;
            InPerSecond = (received - _lastReceived) / seconds;
            OutPerSecond = (sent - _lastSent) / seconds;
            _lastReceived = received;
            _lastSent = sent;
        }

        private void Refresh(GameSession session)
        {
            _labelText.Clear();
            _valueText.Clear();

            Row("FPS", $"{Fps:0}   {FrameMs:0.0} ms   worst {WorstFrameMs:0.0} ms   cap {Application.targetFrameRate}");
            Row("Memory", $"{Mb(GC.GetTotalMemory(false))} managed   {Mb(Profiler.GetTotalAllocatedMemoryLong())} engine");
            Row("GC", $"{GC.CollectionCount(0)} collections");
            Row("Screen", $"{Screen.width}x{Screen.height}   {SystemInfo.graphicsDeviceType}");

            if (session == null)
            {
                Row("Network", "not connected");
            }
            else
            {
                Row("Session", session.Phase.ToString());
                Row("Ping", session.Phase >= SessionPhase.CharacterSelect && session.RoundTripMs > 0 ? $"{session.RoundTripMs:0} ms" : "-");
                Row("Net in", $"{Kb(InPerSecond)}/s   {Mb(session.Connection.BytesReceived)} total");
                Row("Net out", $"{Kb(OutPerSecond)}/s   {Mb(session.Connection.BytesSent)} total");
                var world = session.World;
                if (world.InWorld)
                {
                    var p = world.Predictor.Position;
                    Row("World", $"{world.Info.Name}   tick {world.LastServerTick}");
                    Row("Position", $"{p.X:0.00}, {p.Y:0.00}");
                    Row("Entities", world.Entities.Count.ToString());
                    Row("Prediction", $"{world.Predictor.PendingCount} pending   {world.Predictor.Corrections} corrections");
                }
            }

            _labels.text = _labelText.ToString();
            _values.text = _valueText.ToString();
            _panel.sizeDelta = new Vector2(Width, Padding * 2 + FontSize + 6 + _labels.preferredHeight);
        }

        private void Row(string label, string value)
        {
            if (_labelText.Length > 0)
            {
                _labelText.Append('\n');
                _valueText.Append('\n');
            }
            _labelText.Append(label);
            _valueText.Append(value);
        }

        public static string Mb(long bytes) => $"{bytes / (1024f * 1024f):0.0} MB";
        public static string Kb(float bytes) => $"{bytes / 1024f:0.0} KB";
    }
}
