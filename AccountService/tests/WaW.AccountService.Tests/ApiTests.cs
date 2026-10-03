using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using WaW.AccountService.Accounts;
using WaW.AccountService.Api;

namespace WaW.AccountService.Tests;

public class ApiTests : IDisposable
{
    private readonly ApiFactory _factory = new();
    private readonly HttpClient _http;

    public ApiTests() => _http = _factory.CreateClient();

    public void Dispose() => _factory.Dispose();

    private async Task<LoginResponse> RegisterAndLogin(string name = "Bob", string password = "password123")
    {
        Assert.Equal(HttpStatusCode.Created, (await _http.PostAsJsonAsync("/api/v1/accounts", new RegisterRequest(name, password))).StatusCode);
        var login = await _http.PostAsJsonAsync("/api/v1/sessions", new LoginRequest(name, password));
        Assert.Equal(HttpStatusCode.OK, login.StatusCode);
        return (await login.Content.ReadFromJsonAsync<LoginResponse>())!;
    }

    private HttpRequestMessage Authorized(HttpMethod method, string url, string token)
    {
        var request = new HttpRequestMessage(method, url);
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
        return request;
    }

    [Fact]
    public async Task VersionIsPublic()
    {
        var v = await _http.GetFromJsonAsync<VersionResponse>("/api/v1/version");
        Assert.Equal(1, v!.ProtocolVersion);
    }

    [Fact]
    public async Task RegisterLoginAndReadTheAccount()
    {
        var login = await RegisterAndLogin();
        _factory.Accounts.AddCharacter(login.AccountId, new CharacterSummary(1, 0x0300, 0, 5, 10));

        var response = await _http.SendAsync(Authorized(HttpMethod.Get, "/api/v1/account", login.Token));
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        var account = (await response.Content.ReadFromJsonAsync<AccountResponse>())!;
        Assert.Equal("Bob", account.Name);
        Assert.True(account.StarterPending);
        Assert.Single(account.Characters);
        Assert.Equal(5, account.Characters[0].Level);
    }

    [Fact]
    public async Task InvalidRegistrationsAreRefused()
    {
        Assert.Equal(HttpStatusCode.BadRequest, (await _http.PostAsJsonAsync("/api/v1/accounts", new RegisterRequest("Bob1", "password123"))).StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest, (await _http.PostAsJsonAsync("/api/v1/accounts", new RegisterRequest("Bob", "short"))).StatusCode);
        await RegisterAndLogin();
        var again = await _http.PostAsJsonAsync("/api/v1/accounts", new RegisterRequest("BOB", "password123"));
        Assert.Equal(HttpStatusCode.Conflict, again.StatusCode);
    }

    [Fact]
    public async Task WrongPasswordAndUnknownNameLookTheSame()
    {
        await RegisterAndLogin();
        var wrong = await _http.PostAsJsonAsync("/api/v1/sessions", new LoginRequest("Bob", "password124"));
        var unknown = await _http.PostAsJsonAsync("/api/v1/sessions", new LoginRequest("Nobody", "password123"));
        Assert.Equal(HttpStatusCode.Unauthorized, wrong.StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, unknown.StatusCode);
        Assert.Equal(await wrong.Content.ReadAsStringAsync(), await unknown.Content.ReadAsStringAsync());
    }

    [Fact]
    public async Task LoginIsCaseInsensitiveOnTheName()
    {
        await RegisterAndLogin();
        var login = await _http.PostAsJsonAsync("/api/v1/sessions", new LoginRequest("bOB", "password123"));
        Assert.Equal(HttpStatusCode.OK, login.StatusCode);
    }

    [Fact]
    public async Task ProtectedRoutesNeedAValidToken()
    {
        Assert.Equal(HttpStatusCode.Unauthorized, (await _http.GetAsync("/api/v1/account")).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await _http.SendAsync(Authorized(HttpMethod.Get, "/api/v1/account", "garbage"))).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await _http.PostAsync("/api/v1/game/join", null)).StatusCode);
    }

    [Fact]
    public async Task LogoutEndsTheSession()
    {
        var login = await RegisterAndLogin();
        Assert.Equal(HttpStatusCode.NoContent, (await _http.SendAsync(Authorized(HttpMethod.Delete, "/api/v1/sessions", login.Token))).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await _http.SendAsync(Authorized(HttpMethod.Get, "/api/v1/account", login.Token))).StatusCode);
    }

    [Fact]
    public async Task AJoinTicketIsSingleUseAndNamesTheAccount()
    {
        var login = await RegisterAndLogin();
        var response = await _http.SendAsync(Authorized(HttpMethod.Post, "/api/v1/game/join", login.Token));
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        var join = (await response.Content.ReadFromJsonAsync<JoinResponse>())!;
        Assert.Equal(2050, join.GamePort);
        Assert.NotEqual(login.Token, join.Ticket);

        var consumed = _factory.Sessions.ConsumeJoinTicket(join.Ticket);
        Assert.NotNull(consumed);
        Assert.Equal(login.AccountId, consumed!.AccountId);
        Assert.Null(_factory.Sessions.ConsumeJoinTicket(join.Ticket));
    }

    [Fact]
    public async Task ACharacterCannotBeDeletedWhileTheAccountIsInGame()
    {
        var login = await RegisterAndLogin();
        _factory.Accounts.AddCharacter(login.AccountId, new CharacterSummary(1, 0x0300, 0, 1, 0));
        _factory.Sessions.SetInGame(login.AccountId, true);
        Assert.Equal(HttpStatusCode.Conflict, (await _http.SendAsync(Authorized(HttpMethod.Delete, "/api/v1/characters/1", login.Token))).StatusCode);
        _factory.Sessions.SetInGame(login.AccountId, false);
        Assert.Equal(HttpStatusCode.NoContent, (await _http.SendAsync(Authorized(HttpMethod.Delete, "/api/v1/characters/1", login.Token))).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await _http.SendAsync(Authorized(HttpMethod.Delete, "/api/v1/characters/1", login.Token))).StatusCode);
    }
}

public class RateLimitTests
{
    [Fact]
    public async Task LoginAttemptsAreLimitedPerAddress()
    {
        using var factory = new ApiFactory { AuthLimit = 3 };
        var http = factory.CreateClient();
        var codes = new List<HttpStatusCode>();
        for (var i = 0; i < 5; i++)
            codes.Add((await http.PostAsJsonAsync("/api/v1/sessions", new LoginRequest("Bob", "password123"))).StatusCode);
        Assert.Equal(3, codes.Count(c => c == HttpStatusCode.Unauthorized));
        Assert.Equal(2, codes.Count(c => c == HttpStatusCode.TooManyRequests));
    }
}
