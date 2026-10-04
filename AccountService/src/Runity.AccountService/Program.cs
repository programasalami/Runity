using System.Threading.RateLimiting;
using Microsoft.Extensions.Options;
using Npgsql;
using StackExchange.Redis;
using Runity.AccountService;
using Runity.AccountService.Accounts;
using Runity.AccountService.Api;
using Runity.AccountService.Database;
using Runity.AccountService.Sessions;

// Runity Account/API service.
//   run:      dotnet run --project AccountService/src/Runity.AccountService
//   migrate:  dotnet run --project AccountService/src/Runity.AccountService -- migrate
//   no DB:    dotnet run --project AccountService/src/Runity.AccountService -- --Service:AccountStore=InMemory
//             (sessions + join tickets still go to Redis, so the C++ GameServer can accept players)

var builder = WebApplication.CreateBuilder(args.Where(a => a != "migrate").ToArray());
var options = builder.Configuration.GetSection("Service").Get<ServiceOptions>() ?? new ServiceOptions();
builder.Services.Configure<ServiceOptions>(builder.Configuration.GetSection("Service"));
builder.Services.AddSingleton(new PasswordHasher(options.PasswordIterations));

var usePostgres = options.AccountStore.Equals("Postgres", StringComparison.OrdinalIgnoreCase) || args.Contains("migrate");
if (usePostgres)
{
    builder.Services.AddSingleton(_ => NpgsqlDataSource.Create(ConnInfo.ToNpgsql(options.PgConnInfo)));
    builder.Services.AddSingleton<IAccountStore, PostgresAccountStore>();
}
else if (options.AccountStore.Equals("InMemory", StringComparison.OrdinalIgnoreCase))
{
    builder.Services.AddSingleton<IAccountStore, InMemoryAccountStore>();
}
else
{
    throw new InvalidOperationException($"unknown Service:AccountStore '{options.AccountStore}'");
}

if (options.SessionStore.Equals("InMemory", StringComparison.OrdinalIgnoreCase))
{
    builder.Services.AddSingleton<ISessionStore, InMemorySessionStore>();
}
else if (options.SessionStore.Equals("Redis", StringComparison.OrdinalIgnoreCase))
{
    var redisUrl = Environment.GetEnvironmentVariable("RUNITY_REDIS_URL") ?? "redis://127.0.0.1:6379";
    var redisEndpoint = new Uri(redisUrl);
    builder.Services.AddSingleton<IConnectionMultiplexer>(_ =>
        ConnectionMultiplexer.Connect(new ConfigurationOptions
        {
            EndPoints = { { redisEndpoint.Host, redisEndpoint.Port > 0 ? redisEndpoint.Port : 6379 } },
            AbortOnConnectFail = false,
        }));
    builder.Services.AddSingleton<ISessionStore>(sp =>
        new RedisSessionStore(sp.GetRequiredService<IConnectionMultiplexer>(), options.RedisPrefix));
}
else
{
    throw new InvalidOperationException($"unknown Service:SessionStore '{options.SessionStore}'");
}

builder.Services.AddRateLimiter(limiter =>
{
    limiter.RejectionStatusCode = StatusCodes.Status429TooManyRequests;
    limiter.AddPolicy(ApiEndpoints.AuthRateLimitPolicy, http => RateLimitPartition.GetFixedWindowLimiter(
        http.Connection.RemoteIpAddress?.ToString() ?? "unknown",
        _ => new FixedWindowRateLimiterOptions
        {
            PermitLimit = http.RequestServices.GetRequiredService<IOptions<ServiceOptions>>().Value.AuthRequestsPerMinutePerAddress,
            Window = TimeSpan.FromMinutes(1),
        }));
});

var app = builder.Build();

if (usePostgres)
{
    // Pending migrations are applied at every start, so a fresh database needs no separate step; "migrate" applies them and exits.
    var directory = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, options.MigrationsDirectory));
    if (!Directory.Exists(directory)) directory = Path.GetFullPath(options.MigrationsDirectory);
    var runner = new MigrationRunner(app.Services.GetRequiredService<NpgsqlDataSource>(), directory, Console.WriteLine);
    var applied = await runner.ApplyAsync(CancellationToken.None);
    if (args.Contains("migrate"))
    {
        Console.WriteLine($"{applied} migration(s) applied from {directory}");
        return;
    }
}

app.UseRateLimiter();
app.MapApi();
app.Run();

/// <summary>Visible to the test project's WebApplicationFactory.</summary>
public partial class Program;
