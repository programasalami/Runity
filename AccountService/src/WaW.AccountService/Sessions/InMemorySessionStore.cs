namespace WaW.AccountService.Sessions;

/// <summary>In-memory session store for tests and the database-less development mode. Expiry is checked on read.</summary>
public sealed class InMemorySessionStore : ISessionStore
{
    private readonly object _gate = new();
    private readonly Dictionary<string, (SessionInfo Info, DateTime Expires)> _sessions = new();
    private readonly Dictionary<string, (SessionInfo Info, DateTime Expires)> _tickets = new();
    private readonly HashSet<long> _inGame = new();
    private readonly Func<DateTime> _now;

    public InMemorySessionStore(Func<DateTime>? now = null) => _now = now ?? (() => DateTime.UtcNow);

    public Task<string> CreateSessionAsync(SessionInfo info, TimeSpan ttl)
    {
        var token = Tokens.NewToken();
        lock (_gate) _sessions[Tokens.Hash(token)] = (info, _now() + ttl);
        return Task.FromResult(token);
    }

    public Task<SessionInfo?> GetSessionAsync(string token)
    {
        lock (_gate)
        {
            var key = Tokens.Hash(token);
            if (_sessions.TryGetValue(key, out var s) && s.Expires > _now()) return Task.FromResult<SessionInfo?>(s.Info);
            _sessions.Remove(key);
            return Task.FromResult<SessionInfo?>(null);
        }
    }

    public Task DeleteSessionAsync(string token)
    {
        lock (_gate) _sessions.Remove(Tokens.Hash(token));
        return Task.CompletedTask;
    }

    public Task<string> CreateJoinTicketAsync(SessionInfo info, TimeSpan ttl)
    {
        var ticket = Tokens.NewToken();
        lock (_gate) _tickets[Tokens.Hash(ticket)] = (info, _now() + ttl);
        return Task.FromResult(ticket);
    }

    public Task<bool> IsAccountInGameAsync(long accountId)
    {
        lock (_gate) return Task.FromResult(_inGame.Contains(accountId));
    }

    /// <summary>Test helpers standing in for the GameServer.</summary>
    public SessionInfo? ConsumeJoinTicket(string ticket)
    {
        lock (_gate)
        {
            var key = Tokens.Hash(ticket);
            if (!_tickets.Remove(key, out var t) || t.Expires <= _now()) return null;
            return t.Info;
        }
    }

    public void SetInGame(long accountId, bool inGame)
    {
        lock (_gate)
        {
            if (inGame) _inGame.Add(accountId);
            else _inGame.Remove(accountId);
        }
    }
}
