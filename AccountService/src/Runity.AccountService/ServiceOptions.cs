namespace Runity.AccountService;

/// <summary>Settings (appsettings.json section "Service"). The Redis address comes from RUNITY_REDIS_URL.</summary>
public sealed class ServiceOptions
{
    public string BuildVersion { get; set; } = "0.1.0";
    /// <summary>Must equal protocol_version in Protocol/schema/protocol.toml (a test checks it).</summary>
    public int ProtocolVersion { get; set; } = 1;
    public string GameHost { get; set; } = "127.0.0.1";
    public int GamePort { get; set; } = 2050;
    public int SessionHours { get; set; } = 12;
    public int JoinTicketSeconds { get; set; } = 60;
    public string RedisPrefix { get; set; } = "runity:";
    public int AuthRequestsPerMinutePerAddress { get; set; } = 20;
    public int PasswordIterations { get; set; } = Accounts.PasswordHasher.DefaultIterations;
    /// <summary>"Postgres" (default) or "InMemory" (tests only; accounts vanish on restart).</summary>
    public string AccountStore { get; set; } = "Postgres";
    /// <summary>libpq "key=value" form; must match the GameServer's pgConnInfo. The default is the local development login
    /// (Database/setup/create_database.sql); a public server uses its own password.</summary>
    public string PgConnInfo { get; set; } = "host=localhost port=5432 dbname=runity user=runity password=runitypass";
    /// <summary>"Redis" (default; the GameServer reads join tickets from it) or "InMemory" (tests only: no GameServer can join).</summary>
    public string SessionStore { get; set; } = "Redis";
    /// <summary>Relative to the build output folder (bin/Debug/net10.0), where Runity.AccountService.exe and dotnet run start.</summary>
    public string MigrationsDirectory { get; set; } = "../../../../../../Database/migrations";
}
