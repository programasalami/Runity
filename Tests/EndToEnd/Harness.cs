using System.Diagnostics;
using System.Net;
using System.Net.Http.Json;
using System.Net.Sockets;
using System.Text;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using StackExchange.Redis;
using WaW.AccountService.Api;
using WaW.Protocol;

namespace WaW.EndToEnd.Tests;

public static class Paths
{
    public static string RepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null && !Directory.Exists(Path.Combine(dir.FullName, "Protocol", "schema"))) dir = dir.Parent;
        return dir?.FullName ?? throw new InvalidOperationException("repository root not found");
    }

    public static string ServerExe() =>
        Environment.GetEnvironmentVariable("WAW_GAMESERVER_EXE")
        ?? Path.Combine(RepoRoot(), "Server", "build", "debug", "app", OperatingSystem.IsWindows() ? "waw_gameserver.exe" : "waw_gameserver");
}

/// The Account/API service in-process: accounts in memory, sessions and join tickets in the real Redis under a test prefix.
public sealed class ApiHost : WebApplicationFactory<Program>
{
    private readonly string _prefix;
    public ApiHost(string prefix) => _prefix = prefix;

    protected override void ConfigureWebHost(IWebHostBuilder builder)
    {
        builder.UseSetting("Service:AccountStore", "InMemory");
        builder.UseSetting("Service:SessionStore", "Redis");
        builder.UseSetting("Service:RedisPrefix", _prefix);
        builder.UseSetting("Service:PasswordIterations", "1000");
        builder.UseSetting("Service:AuthRequestsPerMinutePerAddress", "1000");
    }
}

/// The C++ game server as a child process with its own config (port, Redis prefix).
public sealed class ServerProcess : IDisposable
{
    private readonly Process _process;
    private readonly StringBuilder _output = new();
    private readonly string _configPath;
    public int Port { get; }

    public ServerProcess(string redisPrefix, int? runForMs = null, string entryWorld = null)
    {
        var exe = Paths.ServerExe();
        if (!File.Exists(exe)) throw new InvalidOperationException($"game server not built: {exe} (run Server\\build.cmd)");
        Port = FreePort();
        _configPath = Path.Combine(Path.GetTempPath(), $"waw-e2e-{Guid.NewGuid():N}.json");
        var content = Path.Combine(Paths.RepoRoot(), "Content").Replace("\\", "/");
        File.WriteAllText(_configPath, $$"""
            { "bindAddress": "127.0.0.1", "gamePort": {{Port}}, "buildVersion": "e2e", "contentRoot": "{{content}}",
              "serverId": "e2e", "redisPrefix": "{{redisPrefix}}", "helloTimeoutMs": 3000, "statsIntervalMs": 60000,
              "entryWorld": "{{entryWorld ?? ""}}" }
            """);
        var args = $"--config \"{_configPath}\"" + (runForMs is { } ms ? $" --run-for-ms {ms}" : "");
        _process = new Process
        {
            StartInfo = new ProcessStartInfo(exe, args)
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            },
        };
        _process.OutputDataReceived += (_, e) => { lock (_output) _output.AppendLine(e.Data); };
        _process.ErrorDataReceived += (_, e) => { lock (_output) _output.AppendLine(e.Data); };
        _process.Start();
        _process.BeginOutputReadLine();
        _process.BeginErrorReadLine();
        WaitUntil(() => Output.Contains("listening on"), TimeSpan.FromSeconds(20), "the server did not start");
    }

    public string Output
    {
        get { lock (_output) return _output.ToString(); }
    }

    public bool WaitForExit(TimeSpan timeout) => _process.WaitForExit((int)timeout.TotalMilliseconds);
    public int ExitCode => _process.ExitCode;

    public void WaitUntil(Func<bool> condition, TimeSpan timeout, string what)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (condition()) return;
            if (_process.HasExited) throw new InvalidOperationException($"{what}: server exited ({_process.ExitCode})\n{Output}");
            Thread.Sleep(20);
        }
        throw new TimeoutException($"{what}\n{Output}");
    }

    private static int FreePort()
    {
        var l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        var port = ((IPEndPoint)l.LocalEndpoint).Port;
        l.Stop();
        return port;
    }

    public void Dispose()
    {
        if (!_process.HasExited) _process.Kill(entireProcessTree: true);
        _process.Dispose();
        try { File.Delete(_configPath); } catch (IOException) { }
    }
}

/// A scripted game client speaking the protocol over TCP with the Unity client's codec.
public sealed class GameClient : IDisposable
{
    private readonly TcpClient _tcp = new();
    private readonly NetworkStream _stream;
    private readonly FrameDecoder _decoder = new();
    private readonly byte[] _buffer = new byte[64 * 1024];
    public List<IMessage> Received { get; } = new();

    public GameClient(int port)
    {
        _tcp.Connect(IPAddress.Loopback, port);
        _tcp.NoDelay = true;
        _stream = _tcp.GetStream();
    }

    public void SendRaw(byte[] bytes) => _stream.Write(bytes, 0, bytes.Length);

    public void Send(IMessage m)
    {
        var frame = Framing.Encode(m);
        _stream.Write(frame, 0, frame.Length);
    }

    /// Reads until a message of type T arrives (keeping everything received) or the timeout passes.
    public T Expect<T>(TimeSpan? timeout = null, Func<T, bool> match = null) where T : class, IMessage
    {
        var deadline = DateTime.UtcNow + (timeout ?? TimeSpan.FromSeconds(5));
        var start = 0;
        while (true)
        {
            for (; start < Received.Count; start++)
            {
                if (Received[start] is T t && (match is null || match(t))) return t;
            }
            var left = deadline - DateTime.UtcNow;
            if (left <= TimeSpan.Zero) throw new TimeoutException($"no {typeof(T).Name}; got: {string.Join(", ", Received.Select(r => r.Id))}");
            _stream.ReadTimeout = Math.Max(1, (int)left.TotalMilliseconds);
            int n;
            try
            {
                n = _stream.Read(_buffer, 0, _buffer.Length);
            }
            catch (IOException) when (DateTime.UtcNow >= deadline)
            {
                continue;
            }
            if (n == 0) throw new IOException($"connection closed while waiting for {typeof(T).Name}; got: {string.Join(", ", Received.Select(r => r.Id))}");
            _decoder.Feed(_buffer, 0, n);
            while (_decoder.TryNext(out var frame)) Received.Add(MessageCodec.DecodeServer(frame.Id, new ByteReader(frame.Payload)));
        }
    }

    /// True once the server has closed the connection (reads everything still in flight).
    public bool ClosedByServer(TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            _stream.ReadTimeout = 200;
            try
            {
                var n = _stream.Read(_buffer, 0, _buffer.Length);
                if (n == 0) return true;
                _decoder.Feed(_buffer, 0, n);
                while (_decoder.TryNext(out var frame)) Received.Add(MessageCodec.DecodeServer(frame.Id, new ByteReader(frame.Payload)));
            }
            catch (IOException e) when (e.InnerException is SocketException { SocketErrorCode: SocketError.TimedOut })
            {
            }
            catch (IOException)
            {
                return true;
            }
        }
        return false;
    }

    public void Dispose() => _tcp.Dispose();
}

/// One test's world: an isolated Redis prefix, the API service and the game server.
public sealed class Stack : IDisposable
{
    public string Prefix { get; } = $"waw:e2e:{Guid.NewGuid():N}:";
    public ApiHost Api { get; }
    public HttpClient Http { get; }
    public ServerProcess Server { get; }
    public ConnectionMultiplexer Redis { get; } = ConnectionMultiplexer.Connect("127.0.0.1:6379");

    public Stack(int? serverRunForMs = null, string entryWorld = null)
    {
        Api = new ApiHost(Prefix);
        Http = Api.CreateClient();
        Server = new ServerProcess(Prefix, serverRunForMs, entryWorld);
    }

    public async Task<string> RegisterAndLoginAsync(string name, string password = "password123")
    {
        var reg = await Http.PostAsJsonAsync("/api/v1/accounts", new RegisterRequest(name, password));
        if (reg.StatusCode != HttpStatusCode.Created) throw new InvalidOperationException($"register: {reg.StatusCode}");
        var login = await Http.PostAsJsonAsync("/api/v1/sessions", new LoginRequest(name, password));
        login.EnsureSuccessStatusCode();
        return (await login.Content.ReadFromJsonAsync<LoginResponse>())!.Token;
    }

    public async Task<string> JoinTicketAsync(string sessionToken)
    {
        var request = new HttpRequestMessage(HttpMethod.Post, "/api/v1/game/join");
        request.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", sessionToken);
        var response = await Http.SendAsync(request);
        response.EnsureSuccessStatusCode();
        return (await response.Content.ReadFromJsonAsync<JoinResponse>())!.Ticket;
    }

    public GameClient Connect() => new(Server.Port);

    public static Hello Hello(string ticket) =>
        new() { ProtocolVersion = ProtocolInfo.ProtocolVersion, BuildVersion = "e2e", Token = ticket };

    public void Dispose()
    {
        Server.Dispose();
        Http.Dispose();
        Api.Dispose();
        var server = Redis.GetServers()[0];
        foreach (var key in server.Keys(pattern: Prefix + "*")) Redis.GetDatabase().KeyDelete(key);
        Redis.Dispose();
    }
}
