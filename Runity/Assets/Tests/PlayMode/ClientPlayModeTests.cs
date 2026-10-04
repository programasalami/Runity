using System;
using System.Collections;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.TestTools;
using Runity.Client;
using Runity.Presentation.Boot;
using Debug = UnityEngine.Debug;

namespace Runity.PlayMode.Tests
{
    /// <summary>The real client scene against the real services: the Account/API service (built by `dotnet test AccountService`, run
    /// with in-memory accounts) and the C++ game server (built by Server\build.cmd), both as child processes, with Redis on
    /// 127.0.0.1:6379. Saves TestResults/playmode-world.png.</summary>
    public class ClientPlayModeTests
    {
        private sealed class ChildProcess : IDisposable
        {
            private readonly Process _process;
            private readonly StringBuilder _output = new StringBuilder();

            public ChildProcess(string exe, string args, string workingDirectory)
            {
                _process = new Process
                {
                    StartInfo = new ProcessStartInfo(exe, args)
                    {
                        WorkingDirectory = workingDirectory,
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                    },
                };
                _process.OutputDataReceived += (_, e) => { lock (_output) _output.AppendLine(e.Data); };
                _process.ErrorDataReceived += (_, e) => { lock (_output) _output.AppendLine(e.Data); };
                _process.Start();
                _process.BeginOutputReadLine();
                _process.BeginErrorReadLine();
            }

            public string Output
            {
                get { lock (_output) return _output.ToString(); }
            }

            public bool Exited => _process.HasExited;

            public void Dispose()
            {
                try
                {
                    if (!_process.HasExited) _process.Kill();
                }
                catch (InvalidOperationException)
                {
                }
                _process.Dispose();
            }
        }

        private static int FreePort()
        {
            var l = new TcpListener(IPAddress.Loopback, 0);
            l.Start();
            var port = ((IPEndPoint)l.LocalEndpoint).Port;
            l.Stop();
            return port;
        }

        private static string RepoRoot() => Path.GetFullPath(Path.Combine(Application.dataPath, "..", ".."));

        private ChildProcess _api;
        private ChildProcess _server;
        private string _config;

        [TearDown]
        public void TearDown()
        {
            foreach (var v in new[] { "RUNITY_API_URL", "RUNITY_AUTOLOGIN_NAME", "RUNITY_AUTOLOGIN_PASSWORD" }) Environment.SetEnvironmentVariable(v, null);
            _api?.Dispose();
            _server?.Dispose();
            if (_config != null && File.Exists(_config)) File.Delete(_config);
        }

        private static IEnumerator WaitUntil(Func<bool> condition, float seconds, Func<string> what)
        {
            var deadline = Time.realtimeSinceStartup + seconds;
            while (!condition())
            {
                if (Time.realtimeSinceStartup > deadline) Assert.Fail("timed out: " + what());
                yield return null;
            }
        }

        [UnityTest]
        public IEnumerator SignsInEntersTheNexusAndDrawsIt()
        {
            // Editor packages log their own errors in batch mode (e.g. the AI Assistant's relay timing out); this test's verdict comes
            // from its assertions, not from unrelated console errors.
            LogAssert.ignoreFailingMessages = true;
            var root = RepoRoot();
            var serverExe = Path.Combine(root, "Server", "build", "debug", "app", "runity_gameserver.exe");
            var apiDll = Path.Combine(root, "AccountService", "src", "Runity.AccountService", "bin", "Debug", "net10.0", "Runity.AccountService.dll");
            if (!File.Exists(serverExe) || !File.Exists(apiDll)) Assert.Ignore("build the server (Server\\build.cmd) and the API (dotnet build AccountService) first");

            var prefix = $"runity:pm:{Guid.NewGuid():N}:";
            var gamePort = FreePort();
            var apiPort = FreePort();
            _config = Path.Combine(Path.GetTempPath(), $"runity-pm-{Guid.NewGuid():N}.json");
            File.WriteAllText(_config,
                "{ \"bindAddress\": \"127.0.0.1\", \"gamePort\": " + gamePort + ", \"buildVersion\": \"0.1.0\", \"contentRoot\": \"" +
                Path.Combine(root, "Content").Replace("\\", "/") + "\", \"redisPrefix\": \"" + prefix + "\", \"serverId\": \"pm\" }");
            _server = new ChildProcess(serverExe, $"--config \"{_config}\"", Path.GetDirectoryName(serverExe));
            _api = new ChildProcess("dotnet",
                $"\"{apiDll}\" --urls http://127.0.0.1:{apiPort} --Service:AccountStore=InMemory --Service:RedisPrefix={prefix} " +
                $"--Service:PasswordIterations=1000 --Service:GamePort={gamePort} --Service:SessionHours=1 --Logging:LogLevel:Microsoft.AspNetCore=Information",
                Path.GetDirectoryName(apiDll));
            yield return WaitUntil(() => _server.Output.Contains("listening on") || _server.Exited, 90, () => _server.Output);
            yield return WaitUntil(() => _api.Output.Contains("Now listening") || _api.Exited, 90, () => _api.Output);
            Assert.IsFalse(_server.Exited, _server.Output);
            Assert.IsFalse(_api.Exited, _api.Output);

            // Warm the service up: its first request (JIT, first DI resolution) was measured at 10.8 s on a busy machine.
            using (var http = new System.Net.Http.HttpClient { Timeout = TimeSpan.FromSeconds(60) })
            {
                var warm = System.Threading.Tasks.Task.Run(() => http.GetStringAsync($"http://127.0.0.1:{apiPort}/api/v1/version"));
                yield return WaitUntil(() => warm.IsCompleted, 70, () => "the API never answered /api/v1/version\n" + _api.Output);
                Assert.IsFalse(warm.IsFaulted, warm.Exception?.GetBaseException().Message);
            }

            Environment.SetEnvironmentVariable("RUNITY_API_URL", $"http://127.0.0.1:{apiPort}/");
            Environment.SetEnvironmentVariable("RUNITY_AUTOLOGIN_NAME", "Playmode");
            Environment.SetEnvironmentVariable("RUNITY_AUTOLOGIN_PASSWORD", "password123");
            yield return SceneManager.LoadSceneAsync("Game");

            var bootstrap = UnityEngine.Object.FindAnyObjectByType<GameBootstrap>();
            Assert.IsNotNull(bootstrap, "the Game scene has no GameBootstrap");
            yield return WaitUntil(() => bootstrap.Session != null && bootstrap.Session.Phase == SessionPhase.InWorld, 30,
                () => $"phase {bootstrap.Session?.Phase}, status '{bootstrap.LastStatus}', end '{bootstrap.Session?.EndReason}'\n--- server\n{_server.Output}\n--- api\n{_api.Output}");
            yield return WaitUntil(() => bootstrap.WorldView.Tiles.TileCount > 100 && bootstrap.WorldView.Entities.Count > 0, 10,
                () => $"tiles {bootstrap.WorldView.Tiles.TileCount}, entities {bootstrap.WorldView.Entities.Count}");
            yield return new WaitForSecondsRealtime(1f);

            var world = bootstrap.Session.World;
            Assert.AreEqual("Nexus", world.Info.Name);
            Assert.IsTrue(bootstrap.WorldView.Entities.Views.ContainsKey(world.LocalEntityId), "no view for the local player");
            var camPos = Camera.main.transform.position;
            var me = world.Predictor.Position;
            Assert.AreEqual(me.X, camPos.x, 0.5f);
            Assert.AreEqual(-me.Y, camPos.y, 0.5f);

            SaveScreenshot(Path.Combine(root, "TestResults", "playmode-world.png"));
            Debug.Log($"[Runity] play mode: {bootstrap.WorldView.Tiles.TileCount} tiles, {bootstrap.WorldView.Entities.Count} entities");
        }

        private static void SaveScreenshot(string path)
        {
            var cam = Camera.main;
            var rt = new RenderTexture(1280, 720, 24);
            var previous = cam.targetTexture;
            cam.targetTexture = rt;
            cam.Render();
            RenderTexture.active = rt;
            var tex = new Texture2D(rt.width, rt.height, TextureFormat.RGB24, false);
            tex.ReadPixels(new Rect(0, 0, rt.width, rt.height), 0, 0);
            tex.Apply();
            cam.targetTexture = previous;
            RenderTexture.active = null;
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            File.WriteAllBytes(path, tex.EncodeToPNG());
            UnityEngine.Object.Destroy(tex);
            rt.Release();
        }
    }
}
