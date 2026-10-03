using System.Collections.Generic;
using System.Linq;
using NUnit.Framework;
using WaW.Domain.Content;
using WaW.Domain.World;
using WaW.Protocol;

namespace WaW.Client.Tests
{
    public class ClientWorldTests
    {
        private static ContentCatalog _content;
        private static ContentCatalog Content => _content ?? (_content = ContentCatalog.LoadDirectory(TestPaths.Definitions));

        [Test]
        public void TheRealContentLoads()
        {
            var wizard = Content.Class(0x030e);
            Assert.IsNotNull(wizard);
            Assert.AreEqual("Wizard", wizard.Id);
            Assert.AreEqual(10, wizard.StartSpeed);
            Assert.AreEqual(14, Content.Classes.Count);  // the original's 14 classes
            Assert.AreEqual(2, wizard.AnimatedIndex);
            Assert.AreEqual(0xa97, wizard.Equipment[0]);  // Energy Staff
            Assert.IsTrue(Content.Ground(0xFF).NoWalk);
            Assert.IsTrue(Content.Objects.Any(o => o.Class == "Wall" && o.BlocksSight));
        }

        private static ClientWorld InWorld(out uint self)
        {
            var w = new ClientWorld(Content);
            w.Handle(new WorldInfo { WorldId = -1, Name = "Nexus", DisplayName = "Nexus", Width = 8, Height = 8, Music = "" });
            self = 7;
            w.Handle(new PlayerSpawned { EntityId = self, CharacterId = 1, Position = new Protocol.Vec2 { X = 1.5f, Y = 1.5f }, Speed = 10 });
            return w;
        }

        private static EntityFull Full(uint id, float x, float y, string name = "") => new EntityFull
        {
            Id = id, ObjectType = 0x030e, Kind = EntityKind.Player, Position = new Protocol.Vec2 { X = x, Y = y }, Name = name, Hp = 100, MaxHp = 100,
        };

        [Test]
        public void EntitiesEnterChangeAndLeaveWithEvents()
        {
            var w = InWorld(out var self);
            var added = new List<uint>();
            var removed = new List<uint>();
            w.EntityAdded += e => added.Add(e.Id);
            w.EntityRemoved += e => removed.Add(e.Id);

            w.Handle(new Snapshot { ServerTick = 1, AckInputSeq = 0, Entered = new List<EntityFull> { Full(self, 1.5f, 1.5f, "Me"), Full(9, 3.5f, 1.5f, "Amy") },
                Left = new List<uint>(), Changed = new List<EntityDelta>() });
            CollectionAssert.AreEqual(new[] { self, 9u }, added);
            Assert.AreEqual("Amy", w.Find(9).Name);

            w.Handle(new Snapshot { ServerTick = 2, Entered = new List<EntityFull>(), Left = new List<uint>(),
                Changed = new List<EntityDelta> { new EntityDelta { Id = 9, Hp = 40, Position = new Protocol.Vec2 { X = 4f, Y = 1.5f } } } });
            Assert.AreEqual(40, w.Find(9).Hp);
            Assert.AreEqual(100, w.Find(9).MaxHp);
            Assert.AreEqual(4f, w.Find(9).Positions.Latest.X);

            w.Handle(new Snapshot { ServerTick = 3, Entered = new List<EntityFull>(), Left = new List<uint> { 9 }, Changed = new List<EntityDelta>() });
            CollectionAssert.AreEqual(new[] { 9u }, removed);
            Assert.IsNull(w.Find(9));
        }

        [Test]
        public void TilesTakeTheirFlagsFromTheDefinitions()
        {
            var w = InWorld(out _);
            var wall = Content.Objects.First(o => o.Class == "Wall" && o.FullOccupy && o.IsTileObject);
            var water = Content.Grounds.First(g => g.NoWalk && g.Type != 0xFF);
            var grass = Content.Grounds.First(g => !g.NoWalk && !g.Sinking);
            w.Handle(new TileData { Tiles = new List<TileUpdate>
            {
                new TileUpdate { X = 1, Y = 1, GroundType = grass.Type },
                new TileUpdate { X = 2, Y = 1, GroundType = grass.Type, ObjectType = wall.Type },
                new TileUpdate { X = 3, Y = 1, GroundType = water.Type },
            } });
            Assert.IsTrue(w.Map.At(1, 1).Known);
            Assert.AreEqual(0, w.Map.At(1, 1).Flags);
            Assert.AreNotEqual(0, w.Map.At(2, 1).Flags & TileFlags.FullOccupy);
            Assert.AreNotEqual(0, w.Map.At(3, 1).Flags & TileFlags.NoWalk);
            Assert.AreNotEqual(0, w.Map.At(5, 5).Flags & TileFlags.Void);  // not received yet
        }

        [Test]
        public void TheLocalPlayerIsReconciledFromSnapshots()
        {
            var w = InWorld(out var self);
            var grass = Content.Grounds.First(g => !g.NoWalk && !g.Sinking && g.Speed == 1f);
            var tiles = new List<TileUpdate>();
            for (ushort y = 0; y < 8; y++)
                for (ushort x = 0; x < 8; x++) tiles.Add(new TileUpdate { X = x, Y = y, GroundType = grass.Type });
            w.Handle(new TileData { Tiles = tiles });
            w.Handle(new Snapshot { ServerTick = 1, Entered = new List<EntityFull> { Full(self, 1.5f, 1.5f) }, Left = new List<uint>(), Changed = new List<EntityDelta>() });
            w.Predictor.Advance(w.Map, 16f * 3, 127, 0);
            var predicted = w.Predictor.Position.X;
            // The server agrees after step 3.
            w.Handle(new Snapshot { ServerTick = 2, AckInputSeq = 3, Entered = new List<EntityFull>(), Left = new List<uint>(),
                Changed = new List<EntityDelta> { new EntityDelta { Id = self, Position = new Protocol.Vec2 { X = predicted, Y = 1.5f } } } });
            Assert.AreEqual(0f, w.Predictor.LastCorrection);
            Assert.AreEqual(0, w.Predictor.PendingCount);
        }

        [Test]
        public void ChatKeepsTheLastHundredLines()
        {
            var w = InWorld(out _);
            for (var i = 0; i < 120; i++) w.Handle(new ChatMessage { Channel = ChatChannel.Say, SenderName = "A", Text = i.ToString() });
            Assert.AreEqual(ClientWorld.MaxChatLines, w.Chat.Count);
            Assert.AreEqual("119", w.Chat[w.Chat.Count - 1].Text);
        }

        [Test]
        public void ANewWorldClearsTheOldOne()
        {
            var w = InWorld(out var self);
            w.Handle(new Snapshot { ServerTick = 1, Entered = new List<EntityFull> { Full(self, 1.5f, 1.5f) }, Left = new List<uint>(), Changed = new List<EntityDelta>() });
            var removed = 0;
            w.EntityRemoved += _ => removed++;
            w.Handle(new WorldInfo { WorldId = 2, Name = "Realm", DisplayName = "Realm", Width = 4, Height = 4, Music = "" });
            Assert.AreEqual(1, removed);
            Assert.AreEqual(0, w.Entities.Count);
            Assert.IsFalse(w.InWorld);
        }
    }
}
