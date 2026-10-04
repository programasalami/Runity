namespace Runity.AccountService.Accounts;

/// <summary>Thread-safe in-memory account store for tests and for running without a database (--in-memory).</summary>
public sealed class InMemoryAccountStore : IAccountStore
{
    private readonly object _gate = new();
    private readonly Dictionary<long, AccountRecord> _accounts = new();
    private readonly Dictionary<long, List<(CharacterSummary Character, bool Deleted)>> _characters = new();
    private long _nextId = 1;

    public Task<(RegisterResult Result, long AccountId)> RegisterAsync(string name, string passwordHash, string ip, CancellationToken ct)
    {
        lock (_gate)
        {
            if (_accounts.Values.Any(a => string.Equals(a.Name, name, StringComparison.OrdinalIgnoreCase)))
                return Task.FromResult((RegisterResult.NameTaken, 0L));
            var id = _nextId++;
            _accounts[id] = new AccountRecord(id, name, passwordHash, 0, 0, 0, 2, true);
            _characters[id] = new();
            return Task.FromResult((RegisterResult.Created, id));
        }
    }

    public Task<AccountRecord?> FindByNameAsync(string name, CancellationToken ct)
    {
        lock (_gate)
            return Task.FromResult(_accounts.Values.FirstOrDefault(a => string.Equals(a.Name, name, StringComparison.OrdinalIgnoreCase)));
    }

    public Task<AccountRecord?> FindByIdAsync(long accountId, CancellationToken ct)
    {
        lock (_gate)
            return Task.FromResult(_accounts.GetValueOrDefault(accountId));
    }

    public Task<IReadOnlyList<CharacterSummary>> ListLivingCharactersAsync(long accountId, CancellationToken ct)
    {
        lock (_gate)
        {
            IReadOnlyList<CharacterSummary> list = _characters.TryGetValue(accountId, out var chars)
                ? chars.Where(c => !c.Deleted).Select(c => c.Character).OrderBy(c => c.CharacterId).ToList()
                : new List<CharacterSummary>();
            return Task.FromResult(list);
        }
    }

    public Task<DeleteCharacterResult> DeleteCharacterAsync(long accountId, int characterId, CancellationToken ct)
    {
        lock (_gate)
        {
            if (!_characters.TryGetValue(accountId, out var chars)) return Task.FromResult(DeleteCharacterResult.NotFound);
            var index = chars.FindIndex(c => c.Character.CharacterId == characterId && !c.Deleted);
            if (index < 0) return Task.FromResult(DeleteCharacterResult.NotFound);
            chars[index] = (chars[index].Character, true);
            return Task.FromResult(DeleteCharacterResult.Deleted);
        }
    }

    public Task UpdatePasswordHashAsync(long accountId, string passwordHash, CancellationToken ct)
    {
        lock (_gate)
        {
            if (_accounts.TryGetValue(accountId, out var a)) _accounts[accountId] = a with { PasswordHash = passwordHash };
        }
        return Task.CompletedTask;
    }

    /// <summary>Test helper: the GameServer creates characters in production.</summary>
    public void AddCharacter(long accountId, CharacterSummary character)
    {
        lock (_gate) _characters[accountId].Add((character, false));
    }
}
