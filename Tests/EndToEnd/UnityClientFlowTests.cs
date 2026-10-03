using WaW.Client;
using WaW.Domain.Content;
using WaW.Protocol;

namespace WaW.EndToEnd.Tests;

/// The Unity client's own engine-free code (ApiClient, GameSession, ClientWorld, prediction) against the real C++ server.
[Collection("e2e")]
public class UnityClientFlowTests
{
    private static readonly ContentCatalog Content =
        ContentCatalog.LoadDirectory(Path.Combine(Paths.RepoRoot(), "Content", "Definitions"));

    private static void Frames(GameSession session, int count, sbyte dx, sbyte dy, Func<bool> until = null)
    {
        for (var i = 0; i < count; i++)
        {
            session.Update(16.6f, dx, dy);
            if (until != null && until()) return;
            Thread.Sleep(16);
        }
    }

    [Fact]
    public async Task LoginEnterAndWalkWithoutPredictionErrors()
    {
        using var stack = new Stack();
        using var api = new ApiClient("http://localhost/", stack.Api.Server.CreateHandler());
        await api.RegisterAsync("Dee", "password123");
        await api.LoginAsync("Dee", "password123");
        var account = await api.GetAccountAsync();
        Assert.Empty(account.Characters);
        var join = await api.JoinAsync();

        using var session = new GameSession(Content, "e2e");
        await session.StartAsync("127.0.0.1", stack.Server.Port, join.Ticket);
        Frames(session, 200, 0, 0, () => session.Phase == SessionPhase.CharacterSelect);
        Assert.Equal(SessionPhase.CharacterSelect, session.Phase);

        session.CreateCharacter(0x030e);
        Frames(session, 200, 0, 0, () => session.Phase == SessionPhase.InWorld && session.World.LocalPlayer != null);
        Assert.Equal(SessionPhase.InWorld, session.Phase);
        Assert.Equal("Nexus", session.World.Info.Name);
        var start = session.World.Predictor.Position;

        // Walk for ~1.5 s in each direction, then stand still and let the server acknowledge everything.
        var farthest = 0f;
        foreach (var (dx, dy) in new (sbyte, sbyte)[] { (127, 0), (0, 127), (-127, 0), (0, -127) })
        {
            Frames(session, 90, dx, dy);
            var p = session.World.Predictor.Position;
            farthest = Math.Max(farthest, Math.Abs(p.X - start.X) + Math.Abs(p.Y - start.Y));
        }
        Assert.True(farthest > 1f, $"the player barely moved ({farthest} tiles)");
        // Stand still: idle steps keep flowing (the server needs them for sinking ground), so a few are always in flight;
        // once every moving step is acknowledged the server's position equals the prediction exactly.
        var end = session.World.Predictor.Position;
        var self = session.World.LocalPlayer;
        Frames(session, 120, 0, 0, () => self.Positions.Latest.X == end.X && self.Positions.Latest.Y == end.Y);
        Assert.Equal(end.X, self.Positions.Latest.X);
        Assert.Equal(end.Y, self.Positions.Latest.Y);
        Assert.Equal(0, session.World.Predictor.Corrections);  // identical rules: the server never had to correct the client
        Assert.True(session.World.Predictor.PendingCount < 10);

        session.SendChat("unity client says hi");
        Frames(session, 120, 0, 0, () => session.World.Chat.Count > 0);
        Assert.Equal("unity client says hi", session.World.Chat[^1].Text);
        Frames(session, 150, 0, 0, () => session.RoundTripMs > 0);
        Assert.True(session.RoundTripMs > 0);
    }

    [Fact]
    public async Task AFatalFailureEndsTheSessionWithItsReason()
    {
        using var stack = new Stack();
        using var session = new GameSession(Content, "wrong-build");
        await session.StartAsync("127.0.0.1", stack.Server.Port, "no-ticket");
        Frames(session, 200, 0, 0, () => session.Phase == SessionPhase.Closed);
        Assert.Equal(SessionPhase.Closed, session.Phase);
        Assert.Equal(FailureCode.ProtocolMismatch, session.EndCode);
        Assert.Contains("e2e", session.EndReason);
    }
}

[Collection("e2e")]
public class UnityClientCombatTests
{
    private static readonly ContentCatalog Content =
        ContentCatalog.LoadDirectory(Path.Combine(Paths.RepoRoot(), "Content", "Definitions"));

    [Fact]
    public async Task AWizardHuntsRealmMonstersAndEarnsXp()
    {
        using var stack = new Stack(entryWorld: "Realm");
        using var api = new ApiClient("http://localhost/", stack.Api.Server.CreateHandler());
        await api.RegisterAsync("Hunter", "password123");
        await api.LoginAsync("Hunter", "password123");
        var join = await api.JoinAsync();

        using var session = new GameSession(Content, "e2e");
        await session.StartAsync("127.0.0.1", stack.Server.Port, join.Ticket);
        for (var i = 0; i < 300 && session.Phase != SessionPhase.CharacterSelect; i++) { session.Update(16f, 0, 0); Thread.Sleep(10); }
        session.CreateCharacter(0x030e);
        for (var i = 0; i < 300 && !(session.Phase == SessionPhase.InWorld && session.World.Stats != null); i++) { session.Update(16f, 0, 0); Thread.Sleep(10); }
        Assert.Equal("Realm", session.World.Info.Name);
        Assert.Equal(0x0a97, session.World.Items[0]);  // the Energy Staff

        var enemyHurt = false;
        session.World.NotificationReceived += n =>
        {
            if (n.IsDamage && n.EntityId != session.World.LocalEntityId) enemyHurt = true;
        };
        var deadline = DateTime.UtcNow.AddSeconds(90);
        while (DateTime.UtcNow < deadline && session.Phase == SessionPhase.InWorld && (session.World.Stats.Xp == 0 && session.World.Stats.Level == 1))
        {
            var me = session.World.Predictor.Position;
            var target = session.World.Entities.Values.Where(e => e.Kind == WaW.Protocol.EntityKind.Enemy)
                .OrderBy(e => Dist(e.RenderPosition, me)).FirstOrDefault();
            sbyte dx = 0, dy = 0;
            var fire = false;
            var aim = 0f;
            if (target == null)
            {
                // Nothing in sight yet: the realm's spawn is on the shore, walk inland (towards the map's centre) to find some.
                var inland = MathF.Atan2(1024f - me.Y, 1024f - me.X);
                (dx, dy) = (InputMapping.Quantize(MathF.Cos(inland)), InputMapping.Quantize(MathF.Sin(inland)));
            }
            else
            {
                var d = Dist(target.RenderPosition, me);
                aim = MathF.Atan2(target.RenderPosition.Y - me.Y, target.RenderPosition.X - me.X);
                fire = d < 7f;
                if (d > 4f) (dx, dy) = (InputMapping.Quantize(MathF.Cos(aim)), InputMapping.Quantize(MathF.Sin(aim)));
            }
            session.Update(16f, dx, dy, fire, aim);
            Thread.Sleep(16);
        }
        Assert.True(enemyHurt, "no enemy was ever damaged");
        Assert.True(session.World.Stats.Xp > 0 || session.World.Stats.Level > 1,
            $"no XP after 90 s (phase {session.Phase}, {session.EndReason}); enemies seen: {session.World.Entities.Values.Count(e => e.Kind == WaW.Protocol.EntityKind.Enemy)}");
    }

    [Fact]
    public async Task DropAnItemIntoABagAndTakeItBack()
    {
        using var stack = new Stack();
        using var api = new ApiClient("http://localhost/", stack.Api.Server.CreateHandler());
        await api.RegisterAsync("Packer", "password123");
        await api.LoginAsync("Packer", "password123");
        var join = await api.JoinAsync();
        using var session = new GameSession(Content, "e2e");
        await session.StartAsync("127.0.0.1", stack.Server.Port, join.Ticket);
        void Pump(Func<bool> until)
        {
            for (var i = 0; i < 400 && !until(); i++) { session.Update(16f, 0, 0); Thread.Sleep(10); }
        }
        Pump(() => session.Phase == SessionPhase.CharacterSelect);
        session.CreateCharacter(0x030e);
        Pump(() => session.Phase == SessionPhase.InWorld && session.World.Items.Count == 20);
        var spell = session.World.Items[1];
        Assert.Equal(0x0a2e, spell);  // the Fire Spray Spell

        session.DropItem(1);
        Pump(() => session.World.Items[1] == -1 && session.NearestBag() != null);
        var bag = session.NearestBag();
        Assert.NotNull(bag);
        Assert.Equal(spell, bag.Items[0]);

        session.MoveItem(bag.Id, 0, session.World.LocalEntityId, 1);
        Pump(() => session.World.Items[1] == spell && session.NearestBag() == null);
        Assert.Equal(spell, session.World.Items[1]);
        Assert.Null(session.NearestBag());  // the emptied bag is gone
    }

    private static float Dist(WaW.Domain.World.Vec2 a, WaW.Domain.World.Vec2 b) => MathF.Sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y));
}
