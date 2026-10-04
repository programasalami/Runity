using System.Net.Sockets;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Runity.AccountService.Accounts;
using Runity.AccountService.Sessions;

namespace Runity.AccountService.Tests;

/// <summary>Runs only when the local runity_test database answers (Database/setup/create_database.sql makes it).</summary>
public sealed class PostgresFactAttribute : FactAttribute
{
    public PostgresFactAttribute()
    {
        if (!LocalPostgres.Available) Skip = "no local PostgreSQL runity_test database";
    }
}

public static class LocalPostgres
{
    public const string TestConnInfo = "host=localhost port=5432 dbname=runity_test user=runity password=runitypass";
    public static readonly bool Available = Probe();

    private static bool Probe()
    {
        try
        {
            var cs = new Npgsql.NpgsqlConnectionStringBuilder(Database.ConnInfo.ToNpgsql(TestConnInfo)) { Timeout = 2 };
            using var connection = new Npgsql.NpgsqlConnection(cs.ConnectionString);
            connection.Open();
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }
}

/// <summary>Runs only when a Redis server answers on 127.0.0.1:6379 (Memurai on the dev PC).</summary>
public sealed class RedisFactAttribute : FactAttribute
{
    public RedisFactAttribute()
    {
        if (!LocalRedis.Available) Skip = "no Redis on 127.0.0.1:6379";
    }
}

public static class LocalRedis
{
    public static readonly bool Available = Probe();

    private static bool Probe()
    {
        try
        {
            using var client = new TcpClient();
            return client.ConnectAsync("127.0.0.1", 6379).Wait(300) && client.Connected;
        }
        catch (Exception)
        {
            return false;
        }
    }
}

/// <summary>The real HTTP pipeline with in-memory stores and a fast password hash.</summary>
public sealed class ApiFactory : WebApplicationFactory<Program>
{
    public InMemoryAccountStore Accounts { get; } = new();
    public InMemorySessionStore Sessions { get; } = new();
    public int AuthLimit { get; init; } = 1000;

    protected override void ConfigureWebHost(IWebHostBuilder builder)
    {
        builder.UseSetting("Service:AccountStore", "InMemory");
        builder.UseSetting("Service:SessionStore", "InMemory");
        builder.UseSetting("Service:AuthRequestsPerMinutePerAddress", AuthLimit.ToString());
        builder.ConfigureTestServices(services =>
        {
            services.AddSingleton<IAccountStore>(Accounts);
            services.AddSingleton<ISessionStore>(Sessions);
            services.AddSingleton(new PasswordHasher(1000));
        });
    }
}
