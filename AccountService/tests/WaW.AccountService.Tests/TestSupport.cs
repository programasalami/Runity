using System.Net.Sockets;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using WaW.AccountService.Accounts;
using WaW.AccountService.Sessions;

namespace WaW.AccountService.Tests;

/// <summary>Runs only when WAW_PG_TEST_CONNINFO points at a disposable database (Database/setup/create_database.sql makes waw_test).</summary>
public sealed class PostgresFactAttribute : FactAttribute
{
    public PostgresFactAttribute()
    {
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("WAW_PG_TEST_CONNINFO")))
            Skip = "WAW_PG_TEST_CONNINFO is not set";
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
