using System.Text.Json;
using StackExchange.Redis;

namespace Runity.AccountService.Sessions;

/// <summary>Key contract (shared with the C++ GameServer):
///   {prefix}session:{sha256(token)}   -> SessionInfo JSON, TTL = session lifetime
///   {prefix}join:{sha256(ticket)}     -> SessionInfo JSON, TTL 60 s, consumed once by the GameServer (GETDEL)
///   {prefix}lock:account:{id}         -> written by the GameServer while the account plays</summary>
public sealed class RedisSessionStore : ISessionStore
{
    private readonly IDatabase _db;
    private readonly string _prefix;

    public RedisSessionStore(IConnectionMultiplexer redis, string prefix)
    {
        _db = redis.GetDatabase();
        _prefix = prefix;
    }

    public async Task<string> CreateSessionAsync(SessionInfo info, TimeSpan ttl)
    {
        var token = Tokens.NewToken();
        await _db.StringSetAsync(SessionKey(token), JsonSerializer.Serialize(info), ttl);
        return token;
    }

    public async Task<SessionInfo?> GetSessionAsync(string token)
    {
        var value = await _db.StringGetAsync(SessionKey(token));
        return value.IsNullOrEmpty ? null : JsonSerializer.Deserialize<SessionInfo>(value.ToString());
    }

    public Task DeleteSessionAsync(string token) => _db.KeyDeleteAsync(SessionKey(token));

    public async Task<string> CreateJoinTicketAsync(SessionInfo info, TimeSpan ttl)
    {
        var ticket = Tokens.NewToken();
        await _db.StringSetAsync($"{_prefix}join:{Tokens.Hash(ticket)}", JsonSerializer.Serialize(info), ttl);
        return ticket;
    }

    public Task<bool> IsAccountInGameAsync(long accountId) => _db.KeyExistsAsync($"{_prefix}lock:account:{accountId}");

    private RedisKey SessionKey(string token) => $"{_prefix}session:{Tokens.Hash(token)}";
}
