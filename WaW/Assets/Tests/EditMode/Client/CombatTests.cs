using System.Collections.Generic;
using System.Linq;
using NUnit.Framework;
using WaW.Domain.Content;
using WaW.Domain.World;
using Vec2 = WaW.Domain.World.Vec2;

namespace WaW.Client.Tests
{
    public class CombatTests
    {
        [Test]
        public void AttackPeriodMatchesTheServer()
        {
            // Same expectation as Server/tests/test_combat.cpp "reference formulas".
            Assert.AreEqual(357.14f, CombatRules.AttackPeriodMs(15, 1f), 0.5f);
            Assert.AreEqual(CombatRules.AttackPeriodMs(15, 1f) / 2f, CombatRules.AttackPeriodMs(15, 2f), 0.01f);
        }

        [Test]
        public void PathsMatchTheServer()
        {
            var line = new PathSpec { Kind = PathKind.Line, Speed = 10f, LifetimeMs = 1000, Frequency = 1f };
            Assert.AreEqual(5f, ProjectilePaths.Offset(line, 500, 0, 0f).X, 1e-4f);
            var amp = new PathSpec { Kind = PathKind.Amplitude, Speed = 10f, LifetimeMs = 1000, Amplitude = 0.5f, Frequency = 2f };
            Assert.AreEqual(0.5f, ProjectilePaths.Offset(amp, 125, 0, 0f).Y, 1e-4f);
            Assert.AreEqual(-0.5f, ProjectilePaths.Offset(amp, 125, 1, 0f).Y, 1e-4f);
            var boom = new PathSpec { Kind = PathKind.Boomerang, Speed = 10f, LifetimeMs = 1000, Frequency = 1f };
            Assert.AreEqual(0f, ProjectilePaths.Offset(boom, 1000, 0, 0f).X, 1e-4f);
        }

        [Test]
        public void TheStartingWeaponsAreRead()
        {
            var content = ContentCatalog.LoadDirectory(TestPaths.Definitions);
            var staff = content.Item(0xa97);
            Assert.AreEqual("Energy Staff", staff.Id);
            Assert.AreEqual(18f, staff.Projectile.Speed, 1e-4f);
            Assert.AreEqual(475, staff.Projectile.LifetimeMs);
            Assert.AreEqual(PathKind.Amplitude, staff.Projectile.Path);
            Assert.AreEqual(content.ObjectById("Grey Missile").Type, staff.Projectile.ObjectType);
        }

        private static ClientProjectiles Volley(out List<ClientProjectile> ended, float angleStep = 0f, int count = 1)
        {
            var p = new ClientProjectiles();
            var list = new List<ClientProjectile>();
            p.Ended += list.Add;
            ended = list;
            p.AddVolley(1, false, 0, new Vec2(1.5f, 0.5f), 0f, angleStep, count,
                new PathSpec { Kind = PathKind.Line, Speed = 10f, LifetimeMs = 1000, Frequency = 1f }, 0x29, 100);
            return p;
        }

        [Test]
        public void BulletsStopAtWallsEnemiesAndTheEndOfTheirLife()
        {
            var map = MovementTests.FixtureMap(new[] { "..........#....." });
            var p = Volley(out var ended);
            for (var i = 0; i < 100 && p.Live.Count > 0; i++) p.Update(16f, map, new List<Vec2>(), null);
            Assert.AreEqual(1, ended.Count);
            Assert.Less(ended[0].Position.X, 10.6f);  // stopped entering the wall's tile, not after its lifetime (11.5)

            var open = MovementTests.FixtureMap(new[] { "................................" });
            var q = Volley(out var hit);
            for (var i = 0; i < 100 && q.Live.Count > 0; i++) q.Update(16f, open, new List<Vec2> { new Vec2(4.5f, 0.5f) }, null);
            Assert.That(hit[0].Position.X, Is.InRange(3.9f, 4.6f));  // within the 0.5 hit radius of the target at 4.5

            var r = Volley(out var expired);
            for (var i = 0; i < 100 && r.Live.Count > 0; i++) r.Update(16f, open, new List<Vec2>(), null);
            Assert.AreEqual(1, expired.Count);
            Assert.GreaterOrEqual(expired[0].ElapsedMs, 1000f);
        }

        [Test]
        public void AVolleyFansOut()
        {
            var p = Volley(out _, angleStep: 0.2f, count: 3);
            Assert.AreEqual(3, p.Live.Count);
            Assert.AreEqual(0.4f, p.Live[2].Angle, 1e-5f);
            Assert.AreEqual(2u, p.Live[2].BulletId);
        }
    }
}
