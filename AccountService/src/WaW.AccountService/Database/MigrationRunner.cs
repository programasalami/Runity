using System.Security.Cryptography;
using System.Text;
using Npgsql;

namespace WaW.AccountService.Database;

/// <summary>Applies Database/migrations/NNNN_name.sql in order, each in its own transaction, recording version + checksum in
/// schema_migrations. An already-applied file whose content changed is an error (migrations are append-only).
/// Run as a deploy step: <c>dotnet run --project AccountService/src/WaW.AccountService -- migrate</c>.</summary>
public sealed class MigrationRunner
{
    private readonly NpgsqlDataSource _db;
    private readonly string _directory;
    private readonly Action<string> _log;

    public MigrationRunner(NpgsqlDataSource db, string directory, Action<string> log)
    {
        _db = db;
        _directory = directory;
        _log = log;
    }

    public sealed record Migration(int Version, string Name, string Sql, string Checksum);

    public static IReadOnlyList<Migration> Discover(string directory)
    {
        var list = new List<Migration>();
        foreach (var path in Directory.GetFiles(directory, "*.sql"))
        {
            var file = Path.GetFileNameWithoutExtension(path);
            var underscore = file.IndexOf('_');
            if (underscore <= 0 || !int.TryParse(file[..underscore], out var version))
                throw new InvalidOperationException($"migration file name must be NNNN_name.sql: {file}");
            var sql = File.ReadAllText(path).Replace("\r\n", "\n");
            var checksum = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(sql)));
            list.Add(new Migration(version, file[(underscore + 1)..], sql, checksum));
        }
        list.Sort((a, b) => a.Version.CompareTo(b.Version));
        for (var i = 1; i < list.Count; i++)
        {
            if (list[i].Version == list[i - 1].Version) throw new InvalidOperationException($"duplicate migration version {list[i].Version}");
        }
        return list;
    }

    public async Task<int> ApplyAsync(CancellationToken ct)
    {
        await using (var create = _db.CreateCommand(
                         "CREATE TABLE IF NOT EXISTS schema_migrations (version INT PRIMARY KEY, name TEXT NOT NULL, " +
                         "checksum TEXT NOT NULL, applied_at TIMESTAMPTZ NOT NULL DEFAULT now())"))
        {
            await create.ExecuteNonQueryAsync(ct);
        }

        var applied = new Dictionary<int, string>();
        await using (var read = _db.CreateCommand("SELECT version, checksum FROM schema_migrations"))
        await using (var reader = await read.ExecuteReaderAsync(ct))
        {
            while (await reader.ReadAsync(ct)) applied[reader.GetInt32(0)] = reader.GetString(1);
        }

        var count = 0;
        foreach (var m in Discover(_directory))
        {
            if (applied.TryGetValue(m.Version, out var checksum))
            {
                if (checksum != m.Checksum)
                    throw new InvalidOperationException($"migration {m.Version}_{m.Name} was changed after it was applied");
                continue;
            }
            await using var conn = await _db.OpenConnectionAsync(ct);
            await using var tx = await conn.BeginTransactionAsync(ct);
            await using (var cmd = new NpgsqlCommand(m.Sql, conn, tx))
            {
                await cmd.ExecuteNonQueryAsync(ct);
            }
            await using (var record = new NpgsqlCommand(
                             "INSERT INTO schema_migrations (version, name, checksum) VALUES ($1, $2, $3)", conn, tx))
            {
                record.Parameters.AddWithValue(m.Version);
                record.Parameters.AddWithValue(m.Name);
                record.Parameters.AddWithValue(m.Checksum);
                await record.ExecuteNonQueryAsync(ct);
            }
            await tx.CommitAsync(ct);
            _log($"applied migration {m.Version}_{m.Name}");
            count++;
        }
        return count;
    }
}
