using Npgsql;

namespace Runity.AccountService.Accounts;

/// <summary>Account data in PostgreSQL. Every write is one targeted statement whose failure reaches the caller
/// (the reference's whole-document writes and write-behind queue are gone).</summary>
public sealed class PostgresAccountStore : IAccountStore
{
    private const string AccountColumns = "id, name, password_hash, rank, gold, fame, max_characters, starter_pending";
    private readonly NpgsqlDataSource _db;

    public PostgresAccountStore(NpgsqlDataSource db) => _db = db;

    public async Task<(RegisterResult Result, long AccountId)> RegisterAsync(string name, string passwordHash, string ip, CancellationToken ct)
    {
        await using var cmd = _db.CreateCommand(
            "INSERT INTO accounts (name, password_hash, registration_ip) VALUES ($1, $2, $3) " +
            "ON CONFLICT (name_key) DO NOTHING RETURNING id");
        cmd.Parameters.AddWithValue(name);
        cmd.Parameters.AddWithValue(passwordHash);
        cmd.Parameters.AddWithValue(ip);
        var id = await cmd.ExecuteScalarAsync(ct);
        return id is long value ? (RegisterResult.Created, value) : (RegisterResult.NameTaken, 0L);
    }

    public async Task<AccountRecord?> FindByNameAsync(string name, CancellationToken ct)
    {
        await using var cmd = _db.CreateCommand($"SELECT {AccountColumns} FROM accounts WHERE name_key = lower($1)");
        cmd.Parameters.AddWithValue(name);
        return await ReadAccountAsync(cmd, ct);
    }

    public async Task<AccountRecord?> FindByIdAsync(long accountId, CancellationToken ct)
    {
        await using var cmd = _db.CreateCommand($"SELECT {AccountColumns} FROM accounts WHERE id = $1");
        cmd.Parameters.AddWithValue(accountId);
        return await ReadAccountAsync(cmd, ct);
    }

    private static async Task<AccountRecord?> ReadAccountAsync(NpgsqlCommand cmd, CancellationToken ct)
    {
        await using var reader = await cmd.ExecuteReaderAsync(ct);
        if (!await reader.ReadAsync(ct)) return null;
        return new AccountRecord(reader.GetInt64(0), reader.GetString(1), reader.GetString(2), reader.GetInt16(3), reader.GetInt64(4),
            reader.GetInt64(5), reader.GetInt16(6), reader.GetBoolean(7));
    }

    public async Task<IReadOnlyList<CharacterSummary>> ListLivingCharactersAsync(long accountId, CancellationToken ct)
    {
        await using var cmd = _db.CreateCommand(
            "SELECT character_id, class_type, skin_type, level, fame FROM characters " +
            "WHERE account_id = $1 AND NOT is_dead AND NOT is_deleted ORDER BY character_id");
        cmd.Parameters.AddWithValue(accountId);
        await using var reader = await cmd.ExecuteReaderAsync(ct);
        var list = new List<CharacterSummary>();
        while (await reader.ReadAsync(ct))
        {
            list.Add(new CharacterSummary(reader.GetInt32(0), reader.GetInt32(1), reader.GetInt32(2), reader.GetInt32(3),
                reader.GetInt64(4)));
        }
        return list;
    }

    public async Task<DeleteCharacterResult> DeleteCharacterAsync(long accountId, int characterId, CancellationToken ct)
    {
        await using var cmd = _db.CreateCommand(
            "UPDATE characters SET is_deleted = TRUE, updated_at = now() " +
            "WHERE account_id = $1 AND character_id = $2 AND NOT is_deleted AND NOT is_dead");
        cmd.Parameters.AddWithValue(accountId);
        cmd.Parameters.AddWithValue(characterId);
        return await cmd.ExecuteNonQueryAsync(ct) == 1 ? DeleteCharacterResult.Deleted : DeleteCharacterResult.NotFound;
    }

    public async Task UpdatePasswordHashAsync(long accountId, string passwordHash, CancellationToken ct)
    {
        await using var cmd = _db.CreateCommand("UPDATE accounts SET password_hash = $2 WHERE id = $1");
        cmd.Parameters.AddWithValue(accountId);
        cmd.Parameters.AddWithValue(passwordHash);
        await cmd.ExecuteNonQueryAsync(ct);
    }
}
