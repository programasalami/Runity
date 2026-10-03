namespace WaW.AccountService.Accounts;

public sealed record AccountRecord(
    long Id,
    string Name,
    string PasswordHash,
    int Rank,
    long Gold,
    long Fame,
    int MaxCharacters,
    bool StarterPending);

public sealed record CharacterSummary(
    int CharacterId,
    int ClassType,
    int SkinType,
    int Level,
    long Fame);

public enum RegisterResult
{
    Created,
    NameTaken,
}

public enum DeleteCharacterResult
{
    Deleted,
    NotFound,
}

/// <summary>Persistent account data. The PostgreSQL implementation is the store of record; the in-memory one serves tests and
/// a database-less development mode.</summary>
public interface IAccountStore
{
    Task<(RegisterResult Result, long AccountId)> RegisterAsync(string name, string passwordHash, string ip, CancellationToken ct);
    Task<AccountRecord?> FindByNameAsync(string name, CancellationToken ct);
    Task<AccountRecord?> FindByIdAsync(long accountId, CancellationToken ct);
    Task<IReadOnlyList<CharacterSummary>> ListLivingCharactersAsync(long accountId, CancellationToken ct);
    Task<DeleteCharacterResult> DeleteCharacterAsync(long accountId, int characterId, CancellationToken ct);
    Task UpdatePasswordHashAsync(long accountId, string passwordHash, CancellationToken ct);
}
