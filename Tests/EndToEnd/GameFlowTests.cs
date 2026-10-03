using WaW.Protocol;

namespace WaW.EndToEnd.Tests;

/// The migration's end-to-end flow (Your Task: "Connect, Authenticate, Select character, Enter world, Spawn player, Spawn entity,
/// Move, Attack, Receive state, Interact, Save, Disconnect, Reconnect, Reload character"). Steps whose systems are not migrated yet
/// (Attack, Interact, Save to PostgreSQL) are listed in MigrationStatus.md and are not faked here.
[Collection("e2e")]
public class GameFlowTests
{
    private const ushort Wizard = 0x030e;

    [Fact]
    public async Task ConnectAuthenticateCreateEnterMoveChatDisconnectReconnectReload()
    {
        using var stack = new Stack();
        var session = await stack.RegisterAndLoginAsync("Bob");

        int characterId;
        uint entityId;
        long accountId;
        using (var client = stack.Connect())
        {
            // Connect + authenticate with a single-use join ticket.
            client.Send(Stack.Hello(await stack.JoinTicketAsync(session)));
            var ack = client.Expect<HelloAck>();
            accountId = ack.AccountId;
            Assert.Equal("Bob", ack.AccountName);
            Assert.True(await stack.Redis.GetDatabase().KeyExistsAsync($"{stack.Prefix}lock:account:{ack.AccountId}"));

            // Create a character and enter the world.
            client.Send(new CreateCharacter { ClassType = Wizard, SkinType = 0 });
            var world = client.Expect<WorldInfo>();
            Assert.Equal("Nexus", world.Name);
            var spawned = client.Expect<PlayerSpawned>();
            characterId = spawned.CharacterId;
            entityId = spawned.EntityId;

            // Receive state: tiles around the player and a snapshot containing the player and the Nexus's entities.
            Assert.NotEmpty(client.Expect<TileData>().Tiles);
            var first = client.Expect<Snapshot>(match: s => s.Entered.Any(e => e.Id == entityId));
            var self = first.Entered.Single(e => e.Id == entityId);
            Assert.Equal(EntityKind.Player, self.Kind);
            Assert.Equal(Wizard, self.ObjectType);

            // Move: inputs are acknowledged by sequence number.
            uint seq = 0;
            var directions = new (sbyte, sbyte)[] { (127, 0), (-127, 0), (0, 127), (0, -127) };
            var moved = false;
            foreach (var (dx, dy) in directions)
            {
                var input = new MoveInput { Steps = new List<MoveStep>() };
                for (var i = 0; i < 6; i++) input.Steps.Add(new MoveStep { Seq = ++seq, DtMs = 16, DirX = dx, DirY = dy });
                client.Send(input);
                var target = seq;
                var acked = client.Expect<Snapshot>(match: s => s.AckInputSeq == target);
                moved = acked.Changed.Any(c => c.Id == entityId && c.Position != null)
                        || client.Received.OfType<Snapshot>().Any(s => s.Changed.Any(c => c.Id == entityId && c.Position != null));
                if (moved) break;
            }
            Assert.True(moved, "the player never moved");

            // Chat reaches the world (the sender hears itself).
            client.Send(new ChatSend { Text = "hello from the e2e test" });
            var chat = client.Expect<ChatMessage>();
            Assert.Equal("Bob", chat.SenderName);
            Assert.Equal(entityId, chat.SenderId);

            // Ping / pong.
            client.Send(new Ping { ClientTimeMs = 4242 });
            Assert.Equal(4242u, client.Expect<Pong>().ClientTimeMs);
        }

        // Disconnect releases the account lock.
        var lockKey = $"{stack.Prefix}lock:account:{accountId}";
        stack.Server.WaitUntil(() => !stack.Redis.GetDatabase().KeyExists(lockKey), TimeSpan.FromSeconds(5), "the lock was not released");

        // Reconnect and reload the same character.
        using (var client = stack.Connect())
        {
            client.Send(Stack.Hello(await stack.JoinTicketAsync(session)));
            client.Expect<HelloAck>();
            client.Send(new LoadCharacter { CharacterId = characterId });
            var spawned = client.Expect<PlayerSpawned>();
            Assert.Equal(characterId, spawned.CharacterId);
        }
    }

    [Fact]
    public async Task ATicketWorksOnceAndASecondLoginIsRefused()
    {
        using var stack = new Stack();
        var session = await stack.RegisterAndLoginAsync("Amy");
        var ticket = await stack.JoinTicketAsync(session);

        using var first = stack.Connect();
        first.Send(Stack.Hello(ticket));
        first.Expect<HelloAck>();

        using var replay = stack.Connect();
        replay.Send(Stack.Hello(ticket));
        Assert.Equal(FailureCode.InvalidToken, replay.Expect<Failure>().Code);
        Assert.True(replay.ClosedByServer(TimeSpan.FromSeconds(3)));

        using var second = stack.Connect();
        second.Send(Stack.Hello(await stack.JoinTicketAsync(session)));
        var refused = second.Expect<Failure>();
        Assert.Equal(FailureCode.AccountInUse, refused.Code);
        Assert.True(refused.Fatal);
    }

    [Fact]
    public async Task WrongProtocolVersionAndGarbageAreRefused()
    {
        using var stack = new Stack();
        using (var old = stack.Connect())
        {
            old.Send(new Hello { ProtocolVersion = 999, BuildVersion = "e2e", Token = "x" });
            Assert.Equal(FailureCode.ProtocolMismatch, old.Expect<Failure>().Code);
        }
        using var garbage = stack.Connect();
        garbage.SendRaw(new byte[] { 0xFF, 0xFF, 0xFF, 0x7F, 1, 0 });  // a frame claiming ~2 GB
        Assert.True(garbage.ClosedByServer(TimeSpan.FromSeconds(3)));
        await Task.CompletedTask;
    }

    [Fact]
    public async Task ShutdownTellsPlayersAndReleasesLocks()
    {
        using var stack = new Stack(serverRunForMs: 8000);
        var session = await stack.RegisterAndLoginAsync("Cy");
        using var client = stack.Connect();
        client.Send(Stack.Hello(await stack.JoinTicketAsync(session)));
        var ack = client.Expect<HelloAck>();
        var failure = client.Expect<Failure>(TimeSpan.FromSeconds(20));
        Assert.Equal(FailureCode.ServerShutdown, failure.Code);
        Assert.True(stack.Server.WaitForExit(TimeSpan.FromSeconds(10)));
        Assert.Equal(0, stack.Server.ExitCode);
        Assert.False(await stack.Redis.GetDatabase().KeyExistsAsync($"{stack.Prefix}lock:account:{ack.AccountId}"));
        Assert.Contains("shutdown complete", stack.Server.Output);
    }
}

[CollectionDefinition("e2e", DisableParallelization = true)]
public class E2ECollection;
