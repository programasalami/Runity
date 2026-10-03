using System;
using System.Threading;
using System.Threading.Tasks;
using WaW.Domain.Content;
using WaW.Domain.World;
using WaW.Net;
using WaW.Protocol;

namespace WaW.Client
{
    public enum SessionPhase
    {
        Offline,
        Connecting,
        Authenticating,
        CharacterSelect,
        EnteringWorld,
        InWorld,
        Closed,
    }

    /// <summary>One play session on the game server: connect with a join ticket, choose a character, play. Drives the network and
    /// the client world once per frame. Plain C# (no Unity): presentation calls Update() and reads World.</summary>
    public sealed class GameSession : IServerMessageHandler, IDisposable
    {
        public const float PingIntervalMs = 2000f;

        private readonly GameConnection _connection = new GameConnection();
        private readonly string _buildVersion;
        private float _sincePingMs;
        private float _clockMs;
        private float _sinceAttackMs = float.MaxValue;
        private ushort _nextShotId;

        public GameSession(ContentCatalog content, string buildVersion)
        {
            World = new ClientWorld(content);
            _buildVersion = buildVersion;
        }

        public ClientWorld World { get; }
        public SessionPhase Phase { get; private set; } = SessionPhase.Offline;
        /// <summary>Why the session ended (a server Failure message or the connection's close reason).</summary>
        public string EndReason { get; private set; }
        public FailureCode? EndCode { get; private set; }
        public float RoundTripMs { get; private set; }
        public GameConnection Connection => _connection;

        public event Action<SessionPhase> PhaseChanged;

        public async Task StartAsync(string host, int port, string joinTicket, CancellationToken ct = default)
        {
            SetPhase(SessionPhase.Connecting);
            try
            {
                // No ConfigureAwait(false): in Unity the continuation must come back to the main thread (PhaseChanged handlers
                // touch game objects).
                await _connection.ConnectAsync(host, port, TimeSpan.FromSeconds(10), ct);
            }
            catch (Exception e) when (e is TimeoutException || e is System.Net.Sockets.SocketException)
            {
                End(null, e.Message);
                return;
            }
            SetPhase(SessionPhase.Authenticating);
            _connection.Send(new Hello { ProtocolVersion = ProtocolInfo.ProtocolVersion, BuildVersion = _buildVersion, Token = joinTicket });
        }

        public void CreateCharacter(ushort classType)
        {
            if (Phase != SessionPhase.CharacterSelect) return;
            SetPhase(SessionPhase.EnteringWorld);
            _connection.Send(new CreateCharacter { ClassType = classType, SkinType = 0 });
        }

        public void LoadCharacter(int characterId)
        {
            if (Phase != SessionPhase.CharacterSelect) return;
            SetPhase(SessionPhase.EnteringWorld);
            _connection.Send(new LoadCharacter { CharacterId = characterId });
        }

        /// <summary>Enters a portal entity (the server checks the distance and where it leads).</summary>
        public void UsePortal(uint portalEntityId)
        {
            if (Phase == SessionPhase.InWorld) _connection.Send(new UsePortal { EntityId = portalEntityId });
        }

        public const int PlayerSlots = 20;
        public const int FirstInventorySlot = 4;
        public const int BackpackSlot = 12;
        public const float BagReach = 1.4f;  // a little inside the server's 1.5

        /// <summary>Moves an item between slots (the player's own entity, or a bag next to them). The server checks every rule;
        /// the inventory shown always comes from the server's answer.</summary>
        public void MoveItem(uint fromEntity, int fromSlot, uint toEntity, int toSlot)
        {
            if (Phase != SessionPhase.InWorld) return;
            _connection.Send(new InvSwap { FromEntity = fromEntity, FromSlot = (byte)fromSlot, ToEntity = toEntity, ToSlot = (byte)toSlot });
        }

        public void UseItem(int slot)
        {
            if (Phase == SessionPhase.InWorld) _connection.Send(new UseItem { Slot = (byte)slot });
        }

        public void DropItem(int slot)
        {
            if (Phase == SessionPhase.InWorld) _connection.Send(new InvDrop { Slot = (byte)slot });
        }

        /// <summary>The first empty inventory slot (4-11), or -1.</summary>
        public int FreeInventorySlot()
        {
            for (var i = FirstInventorySlot; i < BackpackSlot && i < World.Items.Count; i++)
            {
                if (World.Items[i] < 0) return i;
            }
            return -1;
        }

        /// <summary>The nearest loot bag within reach, if any.</summary>
        public ClientEntity NearestBag()
        {
            if (Phase != SessionPhase.InWorld) return null;
            var me = World.Predictor.Position;
            ClientEntity best = null;
            var bestD = BagReach * BagReach;
            foreach (var e in World.Entities.Values)
            {
                if (!e.IsContainer) continue;
                var dx = e.RenderPosition.X - me.X;
                var dy = e.RenderPosition.Y - me.Y;
                var d = dx * dx + dy * dy;
                if (d <= bestD)
                {
                    bestD = d;
                    best = e;
                }
            }
            return best;
        }

        /// <summary>Back to the Nexus (the reference's Escape).</summary>
        public void ReturnToNexus()
        {
            if (Phase == SessionPhase.InWorld) _connection.Send(new Escape());
        }

        /// <summary>The nearest portal within `range` tiles of the local player, if any.</summary>
        public ClientEntity NearestPortal(float range)
        {
            if (Phase != SessionPhase.InWorld) return null;
            var me = World.Predictor.Position;
            ClientEntity best = null;
            var bestD = range * range;
            foreach (var e in World.Entities.Values)
            {
                if (e.Kind != EntityKind.Portal) continue;
                var dx = e.RenderPosition.X - me.X;
                var dy = e.RenderPosition.Y - me.Y;
                var d = dx * dx + dy * dy;
                if (d <= bestD)
                {
                    bestD = d;
                    best = e;
                }
            }
            return best;
        }

        /// <summary>Fires the weapon in slot 0 if its attack period has passed: the shot is sent and its bullets drawn at once.</summary>
        private void TryFire(float aimAngle)
        {
            var weapon = World.Items.Count > 0 ? World.Content.Item(World.Items[0]) : null;
            if (weapon?.Projectile == null || World.Stats == null) return;
            var period = CombatRules.AttackPeriodMs(World.Stats.Dexterity, weapon.RateOfFire);
            if (_sinceAttackMs < period) return;
            _sinceAttackMs = 0f;
            var shotId = _nextShotId++;
            _connection.Send(new Shoot { ShotId = shotId, ClientTimeMs = (uint)_clockMs, Angle = aimAngle });
            var count = Math.Max(1, weapon.NumProjectiles);
            var step = weapon.ArcGapDegrees * MathF.PI / 180f;
            var pd = weapon.Projectile;
            World.Projectiles.AddVolley(World.LocalEntityId, false, (uint)(shotId * count), World.Predictor.Position,
                aimAngle - step * (count - 1) / 2f, step, count,
                new PathSpec { Kind = pd.Path, Speed = pd.Speed, LifetimeMs = pd.LifetimeMs, Amplitude = pd.Amplitude, Frequency = pd.Frequency },
                pd.ObjectType, pd.Size);
        }

        public void SendChat(string text)
        {
            if (Phase == SessionPhase.InWorld && !string.IsNullOrWhiteSpace(text)) _connection.Send(new ChatSend { Text = text });
        }

        /// <summary>One frame: handle what arrived, predict the local player with the current input, send its steps.</summary>
        public void Update(float frameMs, sbyte moveX, sbyte moveY) => Update(frameMs, moveX, moveY, false, 0f);

        /// <summary>One frame with shooting: `firing` holds the attack button, `aimAngle` (radians, world space) points it.</summary>
        public void Update(float frameMs, sbyte moveX, sbyte moveY, bool firing, float aimAngle)
        {
            _sinceAttackMs = Math.Min(_sinceAttackMs + frameMs, 1e9f);
            _clockMs += frameMs;
            _connection.Poll(this);
            if (Phase > SessionPhase.Connecting && Phase != SessionPhase.Closed && !_connection.IsOpen)
            {
                End(null, _connection.CloseReason ?? "connection closed");
                return;
            }
            if (Phase == SessionPhase.InWorld)
            {
                World.Predictor.Advance(World.Map, frameMs, moveX, moveY);
                var steps = World.Predictor.TakeOutbox();
                if (steps.Count > 0) _connection.Send(new MoveInput { Steps = steps });
                if (firing) TryFire(aimAngle);
                World.UpdateRender(frameMs);
            }
            if (Phase >= SessionPhase.CharacterSelect && Phase != SessionPhase.Closed)
            {
                _sincePingMs += frameMs;
                if (_sincePingMs >= PingIntervalMs)
                {
                    _sincePingMs = 0f;
                    _connection.Send(new Ping { ClientTimeMs = (uint)_clockMs });
                }
            }
        }

        // --- server messages: phase changes here, state in ClientWorld ---

        public void Handle(HelloAck message)
        {
            World.Handle(message);
            SetPhase(SessionPhase.CharacterSelect);
        }

        public void Handle(Failure message)
        {
            World.Handle(message);
            if (message.Fatal) End(message.Code, message.Message);
            else if (Phase == SessionPhase.EnteringWorld) SetPhase(SessionPhase.CharacterSelect);
        }

        public void Handle(Pong message) => RoundTripMs = _clockMs - message.ClientTimeMs;

        public void Handle(WorldInfo message) => World.Handle(message);
        public void Handle(TileData message) => World.Handle(message);

        public void Handle(PlayerSpawned message)
        {
            World.Handle(message);
            SetPhase(SessionPhase.InWorld);
        }

        public void Handle(Snapshot message) => World.Handle(message);
        public void Handle(PlayerStats message) => World.Handle(message);
        public void Handle(Inventory message) => World.Handle(message);
        public void Handle(ProjectileVolley message) => World.Handle(message);

        public void Handle(PlayerDied message)
        {
            World.Handle(message);
            End(null, $"You were killed by {message.KilledBy} at level {message.Level}.");
        }
        public void Handle(ChatMessage message) => World.Handle(message);
        public void Handle(Notification message) => World.Handle(message);

        private void End(FailureCode? code, string reason)
        {
            if (Phase == SessionPhase.Closed) return;
            EndCode = code;
            EndReason = reason;
            _connection.Close(reason);
            SetPhase(SessionPhase.Closed);
        }

        private void SetPhase(SessionPhase phase)
        {
            if (Phase == phase) return;
            Phase = phase;
            PhaseChanged?.Invoke(phase);
        }

        public void Dispose() => _connection.Dispose();
    }
}
