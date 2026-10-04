using Npgsql;

namespace Runity.AccountService.Database;

/// <summary>Both services configure PostgreSQL in libpq "key=value key=value" form (Service:PgConnInfo here, pgConnInfo in the
/// GameServer's config; libpq is what the C++ GameServer uses). This converts it to an Npgsql connection string.</summary>
public static class ConnInfo
{
    public static string ToNpgsql(string conninfo)
    {
        var builder = new NpgsqlConnectionStringBuilder();
        foreach (var part in conninfo.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var eq = part.IndexOf('=');
            if (eq <= 0) throw new FormatException($"conninfo entry is not key=value: '{part}'");
            var key = part[..eq].ToLowerInvariant();
            var value = part[(eq + 1)..];
            switch (key)
            {
                case "host": builder.Host = value; break;
                case "port": builder.Port = int.Parse(value); break;
                case "dbname": builder.Database = value; break;
                case "user": builder.Username = value; break;
                case "password": builder.Password = value; break;
                case "sslmode": builder.SslMode = Enum.Parse<SslMode>(value, ignoreCase: true); break;
                case "connect_timeout": builder.Timeout = int.Parse(value); break;
                default: throw new FormatException($"unsupported conninfo key '{key}'");
            }
        }
        return builder.ConnectionString;
    }
}
