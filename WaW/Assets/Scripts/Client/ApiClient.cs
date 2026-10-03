using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace WaW.Client
{
    public sealed class ApiException : Exception
    {
        public HttpStatusCode Status { get; }
        public ApiException(HttpStatusCode status, string message) : base(message) => Status = status;
    }

    public sealed class CharacterInfo
    {
        [JsonProperty("characterId")] public int CharacterId;
        [JsonProperty("classType")] public int ClassType;
        [JsonProperty("skinType")] public int SkinType;
        [JsonProperty("level")] public int Level;
        [JsonProperty("fame")] public long Fame;
        /// <summary>How many of the 8 stats are at the class maximum (the character card's "n/8").</summary>
        [JsonProperty("statsMaxed")] public int StatsMaxed;
    }

    public sealed class AccountInfo
    {
        [JsonProperty("accountId")] public long AccountId;
        [JsonProperty("name")] public string Name;
        [JsonProperty("rank")] public int Rank;
        [JsonProperty("gold")] public long Gold;
        [JsonProperty("fame")] public long Fame;
        [JsonProperty("maxCharacters")] public int MaxCharacters;
        [JsonProperty("starterPending")] public bool StarterPending;
        [JsonProperty("characters")] public List<CharacterInfo> Characters = new List<CharacterInfo>();
    }

    public sealed class JoinInfo
    {
        [JsonProperty("ticket")] public string Ticket;
        [JsonProperty("gameHost")] public string GameHost;
        [JsonProperty("gamePort")] public int GamePort;
    }

    public sealed class VersionInfo
    {
        [JsonProperty("buildVersion")] public string BuildVersion;
        [JsonProperty("protocolVersion")] public uint ProtocolVersion;
    }

    /// <summary>The Account/API service (AccountService/, routes /api/v1/...). After login only the bearer token is kept; the
    /// password is never stored or resent (the reference sent it with every call).</summary>
    public sealed class ApiClient : IDisposable
    {
        private readonly HttpClient _http;
        public string Token { get; private set; }
        /// <summary>Optional diagnostics sink (the Unity client routes it to the console).</summary>
        public Action<string> Trace { get; set; }

        public ApiClient(string baseUrl, HttpMessageHandler handler = null)
        {
            _http = handler == null ? new HttpClient() : new HttpClient(handler);
            _http.BaseAddress = new Uri(baseUrl.EndsWith("/") ? baseUrl : baseUrl + "/");
            _http.Timeout = TimeSpan.FromSeconds(30);  // a cold service on a busy PC took 10.8 s to answer its first request
        }

        /// <summary>Uses a token handed over by the launcher (WAW_LAUNCH_TOKEN) instead of logging in.</summary>
        public void UseToken(string token) => Token = token;

        public Task<VersionInfo> GetVersionAsync(CancellationToken ct = default) => SendAsync<VersionInfo>(HttpMethod.Get, "api/v1/version", null, false, ct);

        public Task RegisterAsync(string name, string password, CancellationToken ct = default) =>
            SendAsync<object>(HttpMethod.Post, "api/v1/accounts", new { name, password }, false, ct);

        public async Task LoginAsync(string name, string password, CancellationToken ct = default)
        {
            var response = await SendAsync<Dictionary<string, object>>(HttpMethod.Post, "api/v1/sessions", new { name, password }, false, ct);
            Token = (string)response["token"];
        }

        public async Task LogoutAsync(CancellationToken ct = default)
        {
            await SendAsync<object>(HttpMethod.Delete, "api/v1/sessions", null, true, ct);
            Token = null;
        }

        public Task<AccountInfo> GetAccountAsync(CancellationToken ct = default) => SendAsync<AccountInfo>(HttpMethod.Get, "api/v1/account", null, true, ct);

        public Task<JoinInfo> JoinAsync(CancellationToken ct = default) => SendAsync<JoinInfo>(HttpMethod.Post, "api/v1/game/join", null, true, ct);

        public Task DeleteCharacterAsync(int characterId, CancellationToken ct = default) =>
            SendAsync<object>(HttpMethod.Delete, $"api/v1/characters/{characterId}", null, true, ct);

        private async Task<T> SendAsync<T>(HttpMethod method, string path, object body, bool authorized, CancellationToken ct)
        {
            using (var request = new HttpRequestMessage(method, path))
            {
                if (authorized)
                {
                    if (Token == null) throw new ApiException(HttpStatusCode.Unauthorized, "Not signed in.");
                    request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", Token);
                }
                if (body != null) request.Content = new StringContent(JsonConvert.SerializeObject(body), Encoding.UTF8, "application/json");
                HttpResponseMessage response;
                try
                {
                    // Started on a pool thread: in Unity's play mode a request started on the main thread never completed
                    // (AsyncDiagnosticsTests); off the main thread it works on every runtime.
                    Trace?.Invoke($"api {method} {path}: sending");
                    var send = Task.Run(() => _http.SendAsync(request, ct), ct);
                    response = await send.ConfigureAwait(false);
                    Trace?.Invoke($"api {method} {path}: {(int)response.StatusCode}");
                }
                catch (Exception e) when (e is TaskCanceledException && !ct.IsCancellationRequested)
                {
                    Trace?.Invoke($"api {method} {path}: timed out");
                    throw new ApiException(HttpStatusCode.RequestTimeout, "The login server did not answer in time.");
                }
                catch (HttpRequestException e)
                {
                    Trace?.Invoke($"api {method} {path}: {e.Message}");
                    throw new ApiException(HttpStatusCode.ServiceUnavailable, "The login server cannot be reached: " + e.Message);
                }
                using (response)
                {
                    var text = response.Content == null ? "" : await response.Content.ReadAsStringAsync().ConfigureAwait(false);
                    if (!response.IsSuccessStatusCode)
                    {
                        string message = null;
                        try
                        {
                            message = JsonConvert.DeserializeObject<Dictionary<string, string>>(text)?["error"];
                        }
                        catch (JsonException)
                        {
                        }
                        if (response.StatusCode == (HttpStatusCode)429) message = "Too many attempts. Please wait a minute.";
                        throw new ApiException(response.StatusCode, message ?? $"Request failed ({(int)response.StatusCode}).");
                    }
                    return string.IsNullOrEmpty(text) ? default : JsonConvert.DeserializeObject<T>(text);
                }
            }
        }

        public void Dispose() => _http.Dispose();
    }
}
