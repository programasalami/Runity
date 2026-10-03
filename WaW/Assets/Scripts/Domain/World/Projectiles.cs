using System;
using System.Collections.Generic;
using WaW.Domain.Content;

namespace WaW.Domain.World
{
    /// <summary>Client copies of the server's combat formulas (Server/libs/sim/src/rules.cpp), used to pace the local player's own
    /// shots so they appear the moment the button is pressed. The server checks every shot against the same numbers.</summary>
    public static class CombatRules
    {
        /// <summary>Milliseconds between attacks: 1 / (0.0015 + clamp(dex, 0, 200) / 75 * 0.0065) / rateOfFire.</summary>
        public static float AttackPeriodMs(int dexterity, float rateOfFire)
        {
            var dex = (float)Math.Clamp(dexterity, 0, 200);
            var perMs = 0.0015f + dex / 75f * 0.0065f;
            return 1f / perMs / (rateOfFire > 0f ? rateOfFire : 1f);
        }
    }

    public struct PathSpec
    {
        public PathKind Kind;
        public float Speed;  // tiles per second
        public int LifetimeMs;
        public float Amplitude;
        public float Frequency;
    }

    /// <summary>The server's closed-form projectile paths (Server/libs/sim/src/projectile_path.cpp; reference ProjectilePaths).</summary>
    public static class ProjectilePaths
    {
        public static Vec2 Offset(in PathSpec spec, float elapsedMs, uint bulletId, float angle)
        {
            var phase = bulletId % 2 == 0 ? 0f : MathF.PI;
            var t = elapsedMs;
            switch (spec.Kind)
            {
                case PathKind.Boomerang:
                {
                    var half = spec.LifetimeMs / 2f;
                    if (t > half) t = spec.LifetimeMs - t;
                    var d = t * (spec.Speed / 1000f);
                    return new Vec2(d * MathF.Cos(angle), d * MathF.Sin(angle));
                }
                case PathKind.Wavy:
                {
                    var theta = angle + MathF.PI / 64f * MathF.Sin(phase + 6f * MathF.PI * t / 1000f);
                    var d = t * (spec.Speed / 1000f);
                    return new Vec2(d * MathF.Cos(theta), d * MathF.Sin(theta));
                }
                case PathKind.Amplitude:
                {
                    var d = t * (spec.Speed / 1000f);
                    var p = new Vec2(d * MathF.Cos(angle), d * MathF.Sin(angle));
                    var life = spec.LifetimeMs > 0 ? spec.LifetimeMs : 1f;
                    var deflection = spec.Amplitude * MathF.Sin(phase + t / life * spec.Frequency * 2f * MathF.PI);
                    p.X += deflection * MathF.Cos(angle + MathF.PI / 2f);
                    p.Y += deflection * MathF.Sin(angle + MathF.PI / 2f);
                    return p;
                }
                default:
                {
                    var d = t * (spec.Speed / 1000f);
                    return new Vec2(d * MathF.Cos(angle), d * MathF.Sin(angle));
                }
            }
        }
    }

    /// <summary>A bullet on screen. Purely visual: the server decides every hit; the client only stops drawing a bullet where the
    /// server will (walls) or where it visibly struck something.</summary>
    public sealed class ClientProjectile
    {
        public uint OwnerId;
        public bool Enemy;
        public uint BulletId;
        public Vec2 Start;
        public float Angle;
        public PathSpec Path;
        public ushort ProjectileType;
        public int Size;
        public float ElapsedMs;
        public Vec2 Position;
        public Vec2 Previous;
        public bool Done;
    }

    public sealed class ClientProjectiles
    {
        public const float HitRadius = 0.5f;  // the server's WorldRules::hit_radius
        private readonly List<ClientProjectile> _live = new List<ClientProjectile>();

        public IReadOnlyList<ClientProjectile> Live => _live;
        public event Action<ClientProjectile> Spawned;
        public event Action<ClientProjectile> Ended;

        public void Clear()
        {
            foreach (var p in _live) Ended?.Invoke(p);
            _live.Clear();
        }

        /// <summary>Adds a volley: count bullets from `start`, the first at `angle`, each next `angleStep` further.</summary>
        public void AddVolley(uint ownerId, bool enemy, uint firstBulletId, Vec2 start, float angle, float angleStep, int count,
            PathSpec path, ushort projectileType, int size)
        {
            for (var i = 0; i < count; i++)
            {
                var p = new ClientProjectile
                {
                    OwnerId = ownerId,
                    Enemy = enemy,
                    BulletId = (uint)((firstBulletId + i) & 0xFFFF),
                    Start = start,
                    Angle = angle + angleStep * i,
                    Path = path,
                    ProjectileType = projectileType,
                    Size = size,
                    Position = start,
                    Previous = start,
                };
                _live.Add(p);
                Spawned?.Invoke(p);
            }
        }

        /// <summary>Advances every bullet. `targets` are the positions this side's bullets can visibly strike: enemies for player bullets
        /// (excluding none), the local player for enemy bullets.</summary>
        public void Update(float frameMs, ClientTileMap map, IReadOnlyList<Vec2> enemyPositions, Vec2? localPlayer)
        {
            for (var i = _live.Count - 1; i >= 0; i--)
            {
                var p = _live[i];
                p.ElapsedMs += frameMs;
                if (p.ElapsedMs >= p.Path.LifetimeMs)
                {
                    End(i);
                    continue;
                }
                var o = ProjectilePaths.Offset(p.Path, p.ElapsedMs, p.BulletId, p.Angle);
                p.Previous = p.Position;
                p.Position = new Vec2(p.Start.X + o.X, p.Start.Y + o.Y);
                if (StoppedByTile(map, p.Position) || Struck(p, enemyPositions, localPlayer)) End(i);
            }
        }

        private static bool StoppedByTile(ClientTileMap map, Vec2 pos)
        {
            if (map == null) return false;
            if (pos.X < 0f || pos.Y < 0f) return true;
            var t = map.At((int)pos.X, (int)pos.Y);
            if (!t.Known) return false;  // not received yet: let it fly (the server knows)
            return (t.Flags & (TileFlags.Void | TileFlags.EnemyOccupySquare | TileFlags.OccupySquare)) != 0;
        }

        private static bool Struck(ClientProjectile p, IReadOnlyList<Vec2> enemies, Vec2? local)
        {
            if (p.Enemy)
                return local.HasValue && SegmentDistance(p.Previous, p.Position, local.Value) <= HitRadius;
            for (var i = 0; i < enemies.Count; i++)
            {
                if (SegmentDistance(p.Previous, p.Position, enemies[i]) <= HitRadius) return true;
            }
            return false;
        }

        public static float SegmentDistance(Vec2 a, Vec2 b, Vec2 p)
        {
            var dx = b.X - a.X;
            var dy = b.Y - a.Y;
            var lenSq = dx * dx + dy * dy;
            var t = lenSq > 0f ? ((p.X - a.X) * dx + (p.Y - a.Y) * dy) / lenSq : 0f;
            t = Math.Clamp(t, 0f, 1f);
            var cx = a.X + t * dx - p.X;
            var cy = a.Y + t * dy - p.Y;
            return MathF.Sqrt(cx * cx + cy * cy);
        }

        private void End(int index)
        {
            var p = _live[index];
            p.Done = true;
            _live.RemoveAt(index);
            Ended?.Invoke(p);
        }
    }
}
