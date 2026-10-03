using Npgsql;
using StackExchange.Redis;
using WaW.AccountService.Accounts;
using WaW.AccountService.Database;
using WaW.AccountService.Sessions;

namespace WaW.AccountService.Tests;

/// <summary>Real Redis (local Memurai), under a unique key prefix that is removed afterwards.</summary>
public class RedisSessionStoreTests : IDisposable
{
    private readonly string _prefix = $"waw:test:{Guid.NewGuid():N}:";
    private readonly ConnectionMultiplexer? _redis = LocalRedis.Available ? ConnectionMultiplexer.Connect("127.0.0.1:6379") : null;

    public void Dispose()
    {
        if (_redis is null) return;
        var server = _redis.GetServers()[0];
        foreach (var key in server.Keys(pattern: _prefix + "*")) _redis.GetDatabase().KeyDelete(key);
        _redis.Dispose();
    }

    [RedisFact]
    public async Task SessionsRoundTripAndExpire()
    {
        var store = new RedisSessionStore(_redis!, _prefix);
        var token = await store.CreateSessionAsync(new SessionInfo(7, "Bob", 0), TimeSpan.FromMilliseconds(400));
        Assert.Equal(7, (await store.GetSessionAsync(token))!.AccountId);
        Assert.True(_redis!.GetDatabase().KeyExists($"{_prefix}session:{Tokens.Hash(token)}"), "the key must be the token's hash");
        await Task.Delay(900);
        Assert.Null(await store.GetSessionAsync(token));
    }

    [RedisFact]
    public async Task JoinTicketsFollowTheKeyContract()
    {
        var store = new RedisSessionStore(_redis!, _prefix);
        var ticket = await store.CreateJoinTicketAsync(new SessionInfo(9, "Amy", 90), TimeSpan.FromSeconds(60));
        // What the C++ GameServer does: GETDEL on {prefix}join:{sha256(ticket)} and parse the JSON.
        var value = await _redis!.GetDatabase().StringGetDeleteAsync($"{_prefix}join:{Tokens.Hash(ticket)}");
        Assert.Equal("{\"accountId\":9,\"name\":\"Amy\",\"rank\":90}", value.ToString());
        Assert.True((await _redis.GetDatabase().StringGetDeleteAsync($"{_prefix}join:{Tokens.Hash(ticket)}")).IsNull);
    }

    [RedisFact]
    public async Task InGameFollowsTheLockKey()
    {
        var store = new RedisSessionStore(_redis!, _prefix);
        Assert.False(await store.IsAccountInGameAsync(5));
        await _redis!.GetDatabase().StringSetAsync($"{_prefix}lock:account:5", "server/1", TimeSpan.FromSeconds(5));
        Assert.True(await store.IsAccountInGameAsync(5));
    }
}

/// <summary>Real PostgreSQL in a throwaway schema of the waw_test database.</summary>
public class PostgresAccountStoreTests : IAsyncLifetime
{
    private readonly string _schema = $"test_{Guid.NewGuid():N}";
    private NpgsqlDataSource? _db;

    public async Task InitializeAsync()
    {
        var conninfo = Environment.GetEnvironmentVariable("WAW_PG_TEST_CONNINFO");
        if (string.IsNullOrEmpty(conninfo)) return;
        var cs = new NpgsqlConnectionStringBuilder(ConnInfo.ToNpgsql(conninfo)) { SearchPath = _schema };
        _db = NpgsqlDataSource.Create(cs.ConnectionString);
        await using (var cmd = _db.CreateCommand($"CREATE SCHEMA {_schema}")) await cmd.ExecuteNonQueryAsync();
        var root = new DirectoryInfo(AppContext.BaseDirectory);
        while (!Directory.Exists(Path.Combine(root.FullName, "Database", "migrations"))) root = root.Parent!;
        await new MigrationRunner(_db, Path.Combine(root.FullName, "Database", "migrations"), _ => { }).ApplyAsync(CancellationToken.None);
    }

    public async Task DisposeAsync()
    {
        if (_db is null) return;
        await using (var cmd = _db.CreateCommand($"DROP SCHEMA {_schema} CASCADE")) await cmd.ExecuteNonQueryAsync();
        await _db.DisposeAsync();
    }

    [PostgresFact]
    public async Task RegisterFindAndRefuseDuplicateNames()
    {
        var store = new PostgresAccountStore(_db!);
        var (result, id) = await store.RegisterAsync("Bob", "hash", "1.2.3.4", default);
        Assert.Equal(RegisterResult.Created, result);
        Assert.Equal(RegisterResult.NameTaken, (await store.RegisterAsync("BOB", "hash", "", default)).Result);
        var bob = await store.FindByNameAsync("bob", default);
        Assert.Equal(id, bob!.Id);
        Assert.True(bob.StarterPending);
        Assert.Equal(2, bob.MaxCharacters);
    }

    [PostgresFact]
    public async Task CharactersListAndSoftDelete()
    {
        var store = new PostgresAccountStore(_db!);
        var (_, id) = await store.RegisterAsync("Amy", "hash", "", default);
        await using (var cmd = _db!.CreateCommand(
                         "INSERT INTO characters (account_id, character_id, class_type, level) VALUES ($1, 1, 768, 3), ($1, 2, 782, 1)"))
        {
            cmd.Parameters.AddWithValue(id);
            await cmd.ExecuteNonQueryAsync();
        }
        Assert.Equal(2, (await store.ListLivingCharactersAsync(id, default)).Count);
        Assert.Equal(DeleteCharacterResult.Deleted, await store.DeleteCharacterAsync(id, 1, default));
        Assert.Equal(DeleteCharacterResult.NotFound, await store.DeleteCharacterAsync(id, 1, default));
        var left = await store.ListLivingCharactersAsync(id, default);
        Assert.Equal(2, Assert.Single(left).CharacterId);
    }

    [PostgresFact]
    public async Task TheDatabaseEnforcesTheNameRuleToo()
    {
        var store = new PostgresAccountStore(_db!);
        await Assert.ThrowsAsync<PostgresException>(() => store.RegisterAsync("Bob1", "hash", "", default));
    }

    [PostgresFact]
    public async Task MigrationsAreIdempotent()
    {
        var root = new DirectoryInfo(AppContext.BaseDirectory);
        while (!Directory.Exists(Path.Combine(root.FullName, "Database", "migrations"))) root = root.Parent!;
        var applied = await new MigrationRunner(_db!, Path.Combine(root.FullName, "Database", "migrations"), _ => { })
            .ApplyAsync(CancellationToken.None);
        Assert.Equal(0, applied);
    }
}
