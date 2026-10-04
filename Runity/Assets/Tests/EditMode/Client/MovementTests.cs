using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Newtonsoft.Json.Linq;
using NUnit.Framework;
using Runity.Domain.World;
using Runity.Protocol;
using Vec2 = Runity.Domain.World.Vec2;

namespace Runity.Client.Tests
{
    public class MovementTests
    {
        /// <summary>The same legend as the C++ fixtures (Server/tests/sim_fixtures.hpp).</summary>
        public static ClientTileMap FixtureMap(IReadOnlyList<string> rows)
        {
            var map = new ClientTileMap(rows[0].Length, rows.Count);
            for (var y = 0; y < rows.Count; y++)
            {
                for (var x = 0; x < rows[y].Length; x++)
                {
                    var t = new ClientTile { Ground = 1, Speed = 1f, Known = true };
                    switch (rows[y][x])
                    {
                        case '#': t.Object = 0x100; t.Flags = TileFlags.FullOccupy | TileFlags.OccupySquare | TileFlags.BlocksSight; break;
                        case 'o': t.Object = 0x101; t.Flags = TileFlags.OccupySquare; break;
                        case '~': t.Ground = 2; t.Flags = TileFlags.NoWalk; break;
                        case 's': t.Ground = 3; t.Sinking = true; t.Speed = 0.5f; break;
                        case ' ': t.Ground = 0xFF; t.Flags = TileFlags.Void | TileFlags.NoWalk; break;
                    }
                    map.SetRaw(x, y, t);
                }
            }
            return map;
        }

        private static string Bits(float f) => "0x" + BitConverter.SingleToInt32Bits(f).ToString("x8");

        [Test]
        public void MatchesTheServerBitForBit()
        {
            var cases = JObject.Parse(File.ReadAllText(Path.Combine(TestPaths.Vectors, "movement_cases.json")));
            var expected = (JArray)JObject.Parse(File.ReadAllText(Path.Combine(TestPaths.Vectors, "movement.json")))["results"];
            var list = (JArray)cases["cases"];
            Assert.AreEqual(expected.Count, list.Count);
            for (var i = 0; i < list.Count; i++)
            {
                var c = list[i];
                var mapName = c["map"].Value<string>();
                var map = FixtureMap(cases["maps"][mapName].Values<string>().ToList());
                var s = new MoverState { Position = new Vec2(c["start"][0].Value<float>(), c["start"][1].Value<float>()) };
                var speed = c["speed"].Value<int>();
                foreach (var step in c["steps"])
                {
                    var dir = MovementRules.DirectionFromInput((sbyte)step[0].Value<int>(), (sbyte)step[1].Value<int>());
                    var dt = (float)step[2].Value<int>();
                    for (var r = 0; r < step[3].Value<int>(); r++) MovementRules.Step(map, ref s, dir, dt, speed);
                }
                var name = c["name"].Value<string>();
                Assert.AreEqual(expected[i]["xBits"].Value<string>(), Bits(s.Position.X), name + " x");
                Assert.AreEqual(expected[i]["yBits"].Value<string>(), Bits(s.Position.Y), name + " y");
                Assert.AreEqual(expected[i]["sinkBits"].Value<string>(), Bits(s.SinkLevel), name + " sink");
            }
        }

        private static ClientTileMap Open(int width = 64) => FixtureMap(new[] { new string('.', width), new string('.', width) });

        [Test]
        public void PredictionMovesImmediatelyAndQueuesSequencedSteps()
        {
            var map = Open();
            var p = new LocalPlayerPredictor();
            p.Reset(new Vec2(1.5f, 0.5f), 0);
            p.Advance(map, 33f, 127, 0);  // two whole 16 ms steps, 1 ms carried over
            var steps = p.TakeOutbox();
            Assert.AreEqual(2, steps.Count);
            Assert.AreEqual(1u, steps[0].Seq);
            Assert.AreEqual(2u, steps[1].Seq);
            Assert.AreEqual(16, steps[0].DtMs);
            Assert.AreEqual(1.5f + 0.004f * 32, p.Position.X, 1e-5f);
            Assert.AreEqual(0, p.TakeOutbox().Count);
        }

        [Test]
        public void AgreeingServerCausesNoCorrection()
        {
            var map = Open();
            var p = new LocalPlayerPredictor();
            p.Reset(new Vec2(1.5f, 0.5f), 0);
            p.Advance(map, 16f * 5, 127, 0);
            // The server applied the first 3 steps with the same rules.
            var server = new MoverState { Position = new Vec2(1.5f, 0.5f) };
            for (var i = 0; i < 3; i++) MovementRules.Step(map, ref server, MovementRules.DirectionFromInput(127, 0), 16f, 0);
            var predicted = p.Position;
            p.Reconcile(map, 3, server.Position);
            Assert.AreEqual(0f, p.LastCorrection);
            Assert.AreEqual(predicted.X, p.Position.X);
            Assert.AreEqual(2, p.PendingCount);
        }

        [Test]
        public void DisagreeingServerWinsAndUnackedStepsAreReplayed()
        {
            var map = Open();
            var p = new LocalPlayerPredictor();
            p.Reset(new Vec2(1.5f, 0.5f), 0);
            p.Advance(map, 16f * 5, 127, 0);
            p.Reconcile(map, 3, new Vec2(10.5f, 0.5f));  // the server put us elsewhere (e.g. a teleport)
            Assert.Greater(p.LastCorrection, 1f);
            Assert.AreEqual(10.5f + 0.004f * 32, p.Position.X, 1e-4f);
            Assert.AreEqual(1, p.Corrections);
        }

        [Test]
        public void PositionBufferInterpolatesHoldsAndNeverExtrapolates()
        {
            var b = new PositionBuffer();
            b.Add(10, new Vec2(0, 0));
            b.Add(11, new Vec2(1, 0));
            Assert.AreEqual(0.5f, b.Sample(10.5).X, 1e-6f);
            Assert.AreEqual(1f, b.Sample(50).X);   // past the newest: hold
            Assert.AreEqual(0f, b.Sample(2).X);    // before the oldest: oldest
            b.Add(11, new Vec2(9, 9));              // duplicate tick ignored
            Assert.AreEqual(1f, b.Latest.X);
            b.Add(20, new Vec2(2, 0));              // after a pause: stays at 1 until tick 19
            Assert.AreEqual(1f, b.Sample(19).X, 1e-6f);
            Assert.AreEqual(1.5f, b.Sample(19.5).X, 1e-6f);
        }

        [Test]
        public void ServerClockFollowsSnapshotsAndAdvancesWithTime()
        {
            var c = new ServerClock();
            c.OnSnapshot(100);
            Assert.AreEqual(100.0, c.Tick);
            c.Advance(50f);
            Assert.AreEqual(101.0, c.Tick, 1e-9);
            c.OnSnapshot(101);
            Assert.AreEqual(101.0, c.Tick, 1e-9);
            c.OnSnapshot(500);  // a jump (world change): adopted at once
            Assert.AreEqual(500.0, c.Tick);
        }
    }
}
