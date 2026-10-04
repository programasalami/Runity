using Microsoft.Extensions.Options;
using Runity.AccountService.Accounts;
using Runity.AccountService.Sessions;

namespace Runity.AccountService.Api;

public sealed record RegisterRequest(string? Name, string? Password);
public sealed record LoginRequest(string? Name, string? Password);
public sealed record ErrorResponse(string Error);
public sealed record RegisterResponse(long AccountId, string Name);
public sealed record LoginResponse(string Token, long AccountId, string Name, int ExpiresInSeconds);
public sealed record VersionResponse(string BuildVersion, int ProtocolVersion);
public sealed record AccountResponse(long AccountId, string Name, int Rank, long Gold, long Fame, int MaxCharacters, bool StarterPending,
    IReadOnlyList<CharacterSummary> Characters);
public sealed record JoinResponse(string Ticket, string GameHost, int GamePort, int ExpiresInSeconds);

/// <summary>HTTP API v1. Authentication: POST /sessions returns a bearer token; every other account route needs
/// "Authorization: Bearer &lt;token&gt;". Passwords are only ever sent to /accounts and /sessions.</summary>
public static class ApiEndpoints
{
    public const string AuthRateLimitPolicy = "auth";

    public static void MapApi(this WebApplication app)
    {
        var api = app.MapGroup("/api/v1");

        api.MapGet("/version", (IOptions<ServiceOptions> o) => new VersionResponse(o.Value.BuildVersion, o.Value.ProtocolVersion));

        api.MapPost("/accounts", RegisterAsync).RequireRateLimiting(AuthRateLimitPolicy);
        api.MapPost("/sessions", LoginAsync).RequireRateLimiting(AuthRateLimitPolicy);
        api.MapDelete("/sessions", LogoutAsync);
        api.MapGet("/account", GetAccountAsync);
        api.MapPost("/game/join", JoinAsync);
        api.MapDelete("/characters/{characterId:int}", DeleteCharacterAsync);
    }

    private static async Task<IResult> RegisterAsync(RegisterRequest request, HttpContext http, IAccountStore accounts,
        PasswordHasher hasher, CancellationToken ct)
    {
        var error = AccountRules.NameError(request.Name) ?? AccountRules.PasswordError(request.Password);
        if (error is not null) return Results.BadRequest(new ErrorResponse(error));
        var ip = http.Connection.RemoteIpAddress?.ToString() ?? "";
        var (result, id) = await accounts.RegisterAsync(request.Name!, hasher.Hash(request.Password!), ip, ct);
        return result == RegisterResult.Created
            ? Results.Created($"/api/v1/account", new RegisterResponse(id, request.Name!))
            : Results.Conflict(new ErrorResponse("That name is taken."));
    }

    // A fixed hash so an unknown name costs the same time as a wrong password (no account-existence oracle by timing).
    private static string? _dummyHash;

    private static async Task<IResult> LoginAsync(LoginRequest request, IAccountStore accounts, ISessionStore sessions,
        PasswordHasher hasher, IOptions<ServiceOptions> options, CancellationToken ct)
    {
        if (string.IsNullOrEmpty(request.Name) || string.IsNullOrEmpty(request.Password))
            return Results.BadRequest(new ErrorResponse("Name and password are required."));
        var account = await accounts.FindByNameAsync(request.Name, ct);
        if (account is null)
        {
            _dummyHash ??= hasher.Hash("not-a-real-password");
            hasher.Verify(request.Password, _dummyHash);
            return Unauthorized();
        }
        if (!hasher.Verify(request.Password, account.PasswordHash)) return Unauthorized();

        var ttl = TimeSpan.FromHours(options.Value.SessionHours);
        var token = await sessions.CreateSessionAsync(new SessionInfo(account.Id, account.Name, account.Rank), ttl);
        return Results.Ok(new LoginResponse(token, account.Id, account.Name, (int)ttl.TotalSeconds));
    }

    private static async Task<IResult> LogoutAsync(HttpContext http, ISessionStore sessions)
    {
        var token = BearerToken(http);
        if (token is null) return Unauthorized();
        await sessions.DeleteSessionAsync(token);
        return Results.NoContent();
    }

    private static async Task<IResult> GetAccountAsync(HttpContext http, ISessionStore sessions, IAccountStore accounts, CancellationToken ct)
    {
        var session = await AuthenticateAsync(http, sessions);
        if (session is null) return Unauthorized();
        var account = await accounts.FindByIdAsync(session.AccountId, ct);
        if (account is null) return Unauthorized();
        var characters = await accounts.ListLivingCharactersAsync(account.Id, ct);
        return Results.Ok(new AccountResponse(account.Id, account.Name, account.Rank, account.Gold, account.Fame, account.MaxCharacters,
            account.StarterPending, characters));
    }

    private static async Task<IResult> JoinAsync(HttpContext http, ISessionStore sessions, IOptions<ServiceOptions> options)
    {
        var session = await AuthenticateAsync(http, sessions);
        if (session is null) return Unauthorized();
        var ttl = TimeSpan.FromSeconds(options.Value.JoinTicketSeconds);
        var ticket = await sessions.CreateJoinTicketAsync(session, ttl);
        return Results.Ok(new JoinResponse(ticket, options.Value.GameHost, options.Value.GamePort, (int)ttl.TotalSeconds));
    }

    private static async Task<IResult> DeleteCharacterAsync(int characterId, HttpContext http, ISessionStore sessions,
        IAccountStore accounts, CancellationToken ct)
    {
        var session = await AuthenticateAsync(http, sessions);
        if (session is null) return Unauthorized();
        // The GameServer owns a character while the account is in game; deleting it then would race with its saves.
        if (await sessions.IsAccountInGameAsync(session.AccountId))
            return Results.Conflict(new ErrorResponse("Leave the game before deleting a character."));
        return await accounts.DeleteCharacterAsync(session.AccountId, characterId, ct) == DeleteCharacterResult.Deleted
            ? Results.NoContent()
            : Results.NotFound(new ErrorResponse("No such character."));
    }

    private static IResult Unauthorized() => Results.Json(new ErrorResponse("Invalid name or password, or the session expired."),
        statusCode: StatusCodes.Status401Unauthorized);

    private static string? BearerToken(HttpContext http)
    {
        var header = http.Request.Headers.Authorization.ToString();
        const string prefix = "Bearer ";
        return header.StartsWith(prefix, StringComparison.Ordinal) && header.Length > prefix.Length ? header[prefix.Length..].Trim() : null;
    }

    private static async Task<SessionInfo?> AuthenticateAsync(HttpContext http, ISessionStore sessions)
    {
        var token = BearerToken(http);
        return token is null ? null : await sessions.GetSessionAsync(token);
    }
}
