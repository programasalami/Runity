using System;
using System.Collections.Generic;
using Runity.Domain.Content;
using Runity.Protocol;
using ContentPathKind = Runity.Domain.Content.PathKind;

namespace Runity.Domain.World
{
    /// <summary>What the client knows about one entity. Not a GameObject: presentation creates a view for it and reads it.</summary>
    public sealed class ClientEntity
    {
        public uint Id;
        public ushort ObjectType;
        public EntityKind Kind;
        public string Name = "";
        public int Hp;
        public int MaxHp;
        public int Level;
        public ushort SkinType;
        public ulong Conditions;
        /// <summary>A container's slots (loot bags; -1 = empty). Empty for everything else.</summary>
        public List<int> Items = new List<int>();
        public bool IsContainer => Kind == EntityKind.Container;
        public readonly PositionBuffer Positions = new PositionBuffer();
        /// <summary>Where presentation should draw it this frame (set by ClientWorld.UpdateRender).</summary>
        public Vec2 RenderPosition;
    }

    public sealed class ChatLine
    {
        public ChatChannel Channel;
        public string Sender;
        public string Text;
    }

    /// <summary>The client-side game state, built only from server messages. Plain C#: it knows nothing about Unity.</summary>
    public sealed class ClientWorld : IServerMessageHandler
    {
        /// <summary>Remote entities are drawn this many ticks in the past (two samples to blend; ~100 ms at 20 TPS).</summary>
        public const double InterpolationDelayTicks = 2.0;
        public const int MaxChatLines = 100;

        private readonly ContentCatalog _content;
        private readonly Dictionary<uint, ClientEntity> _entities = new Dictionary<uint, ClientEntity>();
        private readonly List<ChatLine> _chat = new List<ChatLine>();
        private Vec2 _serverSelfPosition;

        public ClientWorld(ContentCatalog content)
        {
            _content = content;
        }

        public ContentCatalog Content => _content;
        public WorldInfo Info { get; private set; }
        public ClientTileMap Map { get; private set; }
        public uint LocalEntityId { get; private set; }
        public int LocalCharacterId { get; private set; }
        public bool InWorld => Map != null && LocalEntityId != 0;
        public LocalPlayerPredictor Predictor { get; } = new LocalPlayerPredictor();
        public ServerClock Clock { get; } = new ServerClock();
        public IReadOnlyDictionary<uint, ClientEntity> Entities => _entities;
        public IReadOnlyList<ChatLine> Chat => _chat;
        public uint LastServerTick { get; private set; }
        public Failure LastFailure { get; private set; }
        public HelloAck Session { get; private set; }
        /// <summary>The local player's own numbers (PlayerStats), null until the server sends them.</summary>
        public PlayerStats Stats { get; private set; }
        /// <summary>The local player's item slots (-1 = empty).</summary>
        public IReadOnlyList<int> Items { get; private set; } = Array.Empty<int>();
        public ClientProjectiles Projectiles { get; } = new ClientProjectiles();
        public PlayerDied Death { get; private set; }
        private readonly List<Vec2> _enemyPositions = new List<Vec2>();

        public event Action<WorldInfo> WorldChanged;
        public event Action<ClientEntity> EntityAdded;
        public event Action<ClientEntity> EntityRemoved;
        public event Action<IReadOnlyList<TileUpdate>> TilesReceived;
        public event Action<ChatLine> ChatReceived;
        public event Action<Notification> NotificationReceived;
        public event Action<Failure> FailureReceived;
        public event Action<PlayerStats> StatsChanged;
        public event Action<ClientEntity> ContainerChanged;
        public event Action InventoryChanged;
        public event Action<PlayerDied> Died;

        public ClientEntity Find(uint id) => _entities.TryGetValue(id, out var e) ? e : null;
        public ClientEntity LocalPlayer => Find(LocalEntityId);

        public void Handle(HelloAck message) => Session = message;

        public void Handle(Failure message)
        {
            LastFailure = message;
            FailureReceived?.Invoke(message);
        }

        public void Handle(Pong message) { }

        public void Handle(WorldInfo message)
        {
            foreach (var e in _entities.Values) EntityRemoved?.Invoke(e);
            _entities.Clear();
            Projectiles.Clear();
            Info = message;
            Map = new ClientTileMap(message.Width, message.Height);
            LocalEntityId = 0;
            WorldChanged?.Invoke(message);
        }

        public void Handle(TileData message)
        {
            if (Map == null) return;
            foreach (var t in message.Tiles) Map.Set(t.X, t.Y, t.GroundType, t.ObjectType, _content);
            TilesReceived?.Invoke(message.Tiles);
        }

        public void Handle(PlayerSpawned message)
        {
            LocalEntityId = message.EntityId;
            LocalCharacterId = message.CharacterId;
            _serverSelfPosition = new Vec2(message.Position.X, message.Position.Y);
            Predictor.Reset(_serverSelfPosition, message.Speed);
        }

        public void Handle(Snapshot message)
        {
            LastServerTick = message.ServerTick;
            Clock.OnSnapshot(message.ServerTick);
            foreach (var full in message.Entered)
            {
                var e = new ClientEntity
                {
                    Id = full.Id,
                    ObjectType = full.ObjectType,
                    Kind = full.Kind,
                    Name = full.Name,
                    Hp = full.Hp,
                    MaxHp = full.MaxHp,
                    Level = full.Level,
                    SkinType = full.SkinType,
                    Conditions = full.Conditions,
                    Items = full.Items ?? new List<int>(),
                };
                var pos = new Vec2(full.Position.X, full.Position.Y);
                e.Positions.Add(message.ServerTick, pos);
                e.RenderPosition = pos;
                if (e.Id == LocalEntityId) _serverSelfPosition = pos;
                if (_entities.TryGetValue(e.Id, out var stale)) EntityRemoved?.Invoke(stale);
                _entities[e.Id] = e;
                EntityAdded?.Invoke(e);
            }
            foreach (var d in message.Changed)
            {
                if (!_entities.TryGetValue(d.Id, out var e)) continue;
                if (d.Position != null)
                {
                    var pos = new Vec2(d.Position.X, d.Position.Y);
                    e.Positions.Add(message.ServerTick, pos);
                    if (e.Id == LocalEntityId) _serverSelfPosition = pos;
                }
                if (d.Hp.HasValue) e.Hp = d.Hp.Value;
                if (d.MaxHp.HasValue) e.MaxHp = d.MaxHp.Value;
                if (d.Level.HasValue) e.Level = d.Level.Value;
                if (d.SkinType.HasValue) e.SkinType = d.SkinType.Value;
                if (d.Conditions.HasValue) e.Conditions = d.Conditions.Value;
                if (d.Name != null) e.Name = d.Name;
                if (d.Items != null)
                {
                    e.Items = d.Items;
                    ContainerChanged?.Invoke(e);
                }
            }
            foreach (var id in message.Left)
            {
                if (_entities.TryGetValue(id, out var e))
                {
                    _entities.Remove(id);
                    EntityRemoved?.Invoke(e);
                }
            }
            if (InWorld) Predictor.Reconcile(Map, message.AckInputSeq, _serverSelfPosition);
        }

        public void Handle(ChatMessage message)
        {
            var line = new ChatLine { Channel = message.Channel, Sender = message.SenderName, Text = message.Text };
            _chat.Add(line);
            if (_chat.Count > MaxChatLines) _chat.RemoveAt(0);
            ChatReceived?.Invoke(line);
        }

        public void Handle(Notification message) => NotificationReceived?.Invoke(message);

        public void Handle(PlayerStats message)
        {
            var speedChanged = Stats == null || Stats.Speed != message.Speed;
            Stats = message;
            if (speedChanged) Predictor.SetSpeed(message.Speed);
            StatsChanged?.Invoke(message);
        }

        public void Handle(Inventory message)
        {
            Items = message.Items;
            InventoryChanged?.Invoke();
        }

        public void Handle(ProjectileVolley message)
        {
            Projectiles.AddVolley(message.OwnerId, message.OwnerIsEnemy, message.FirstBulletId,
                new Vec2(message.Position.X, message.Position.Y), message.Angle, message.AngleStep, message.Count,
                ToPath(message.Spec), message.Spec.ProjectileType, message.Spec.Size);
        }

        public void Handle(PlayerDied message)
        {
            Death = message;
            Died?.Invoke(message);
        }

        public static PathSpec ToPath(ProjectileSpec spec) => new PathSpec
        {
            Kind = (ContentPathKind)(byte)spec.Path,
            Speed = spec.Speed,
            LifetimeMs = spec.LifetimeMs,
            Amplitude = spec.Amplitude,
            Frequency = spec.Frequency,
        };

        /// <summary>Per frame: advance the server clock and place every entity for drawing.</summary>
        public void UpdateRender(float frameMs)
        {
            Clock.Advance(frameMs);
            var renderTick = Clock.Tick - InterpolationDelayTicks;
            _enemyPositions.Clear();
            foreach (var e in _entities.Values)
            {
                e.RenderPosition = e.Id == LocalEntityId ? Predictor.RenderPosition() : e.Positions.Sample(renderTick);
                if (e.Kind == EntityKind.Enemy) _enemyPositions.Add(e.RenderPosition);
            }
            Vec2? me = InWorld ? Predictor.RenderPosition() : (Vec2?)null;
            Projectiles.Update(frameMs, Map, _enemyPositions, me);
        }
    }
}
