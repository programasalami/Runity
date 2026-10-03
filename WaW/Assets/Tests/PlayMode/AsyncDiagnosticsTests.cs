using System.Collections;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using NUnit.Framework;
using UnityEngine;
using UnityEngine.TestTools;

namespace WaW.PlayMode.Tests
{
    /// <summary>Checks the runtime facilities the client relies on, inside Unity's play mode.</summary>
    public class AsyncDiagnosticsTests
    {
        private static IEnumerator Wait(Task t, float seconds)
        {
            var deadline = Time.realtimeSinceStartup + seconds;
            while (!t.IsCompleted && Time.realtimeSinceStartup < deadline) yield return null;
        }

        [UnityTest]
        public IEnumerator AwaitContinuationsRunOnTheMainThread()
        {
            var main = Thread.CurrentThread.ManagedThreadId;
            var resumedOn = -1;
            async Task Body()
            {
                await Task.Delay(50);
                resumedOn = Thread.CurrentThread.ManagedThreadId;
            }
            var t = Body();
            yield return Wait(t, 5);
            Assert.IsTrue(t.IsCompleted, "an awaited Task.Delay never resumed (synchronization context not pumped)");
            Assert.AreEqual(main, resumedOn);
        }

        [UnityTest]
        public IEnumerator HttpClientReachesALocalListener() => Fetch(startOnMainThread: false);

        [UnityTest]
        public IEnumerator HttpClientStartedOnTheMainThreadReachesALocalListener() => Fetch(startOnMainThread: true);

        private static IEnumerator Fetch(bool startOnMainThread)
        {
            var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            var port = ((IPEndPoint)listener.LocalEndpoint).Port;
            var server = Task.Run(() =>
            {
                using (var c = listener.AcceptTcpClient())
                {
                    var s = c.GetStream();
                    var buf = new byte[4096];
                    s.Read(buf, 0, buf.Length);
                    var reply = System.Text.Encoding.ASCII.GetBytes("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
                    s.Write(reply, 0, reply.Length);
                }
            });
            var http = new HttpClient { Timeout = System.TimeSpan.FromSeconds(5) };
            var url = $"http://127.0.0.1:{port}/";
            var request = startOnMainThread ? http.GetStringAsync(url) : Task.Run(() => http.GetStringAsync(url));
            yield return Wait(request, 10);
            listener.Stop();
            Assert.IsTrue(request.IsCompleted, "HttpClient never completed (not even its timeout)");
            Assert.IsFalse(request.IsFaulted, request.Exception?.GetBaseException().ToString());
            Assert.AreEqual("ok", request.Result);
            server.Wait(1000);
        }
    }
}
