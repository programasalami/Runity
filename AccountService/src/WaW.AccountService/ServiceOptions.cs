namespace WaW.AccountService;

/// <summary>Non-secret settings (appsettings.json section "Service"). Secrets come from WAW_PG_CONNINFO / WAW_REDIS_URL.</summary>
public sealed class ServiceOptions
{
    public string BuildVersion { get; set; } = "0.1.0";
    /// <summary>Must equal protocol_version in Protocol/schema/protocol.toml (a test checks it).</summary>
    public int ProtocolVersion { get; set; } = 1;
    public string GameHost { get; set; } = "127.0.0.1";
    public int GamePort { get; set; } = 2050;
    public int SessionHours { get; set; } = 12;
    public int JoinTicketSeconds { get; set; } = 60;
    public string RedisPrefix { get; set; } = "waw:";
    public int AuthRequestsPerMinutePerAddress { get; set; } = 20;
    public int PasswordIterations { get; set; } = Accounts.PasswordHasher.DefaultIterations;
    /// <summary>"Postgres" (default) or "InMemory" (development and tests only; accounts vanish on restart).</summary>
    public string AccountStore { get; set; } = "Postgres";
    /// <summary>"Redis" (default; the GameServer reads join tickets from it) or "InMemory" (tests only: no GameServer can join).</summary>
    public string SessionStore { get; set; } = "Redis";
    public string MigrationsDirectory { get; set; } = "../../../Database/migrations";
}
