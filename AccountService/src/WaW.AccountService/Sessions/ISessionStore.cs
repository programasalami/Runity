using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Serialization;

namespace WaW.AccountService.Sessions;

/// <summary>Who a token belongs to. Serialized as JSON into Redis and read by the C++ GameServer for join tickets, so the
/// field names are part of the contract documented in Documentation/Migration/Redis.md (section "New key contract").</summary>
public sealed record SessionInfo(
    [property: JsonPropertyName("accountId")] long AccountId,
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("rank")] int Rank);

/// <summary>Transient authentication state (Redis in production). Tokens are random; only their SHA-256 is stored as a key.</summary>
public interface ISessionStore
{
    /// <summary>Creates an API session; returns the bearer token.</summary>
    Task<string> CreateSessionAsync(SessionInfo info, TimeSpan ttl);
    Task<SessionInfo?> GetSessionAsync(string token);
    Task DeleteSessionAsync(string token);
    /// <summary>Creates a single-use game join ticket; the GameServer consumes it with GETDEL.</summary>
    Task<string> CreateJoinTicketAsync(SessionInfo info, TimeSpan ttl);
    /// <summary>True while a GameServer holds the account's play lock.</summary>
    Task<bool> IsAccountInGameAsync(long accountId);
}

public static class Tokens
{
    /// <summary>32 random bytes, base64url without padding.</summary>
    public static string NewToken() =>
        Convert.ToBase64String(RandomNumberGenerator.GetBytes(32)).TrimEnd('=').Replace('+', '-').Replace('/', '_');

    /// <summary>Lower-case hex SHA-256 of the token's UTF-8 bytes (the Redis key part; the C++ server computes the same).</summary>
    public static string Hash(string token) => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(token)));
}
