using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using NUnit.Framework;
using Runity.Net;
using Runity.Protocol;

namespace Runity.Client.Tests
{
    public class NetTests
    {
        private sealed class Recorder : IServerMessageHandler
        {
            public readonly List<IMessage> Got = new List<IMessage>();
            public void Handle(HelloAck m) => Got.Add(m);
            public void Handle(Failure m) => Got.Add(m);
            public void Handle(Pong m) => Got.Add(m);
            public void Handle(WorldInfo m) => Got.Add(m);
            public void Handle(TileData m) => Got.Add(m);
            public void Handle(PlayerSpawned m) => Got.Add(m);
            public void Handle(Snapshot m) => Got.Add(m);
            public void Handle(ChatMessage m) => Got.Add(m);
            public void Handle(Notification m) => Got.Add(m);
            public void Handle(PlayerStats m) => Got.Add(m);
            public void Handle(Inventory m) => Got.Add(m);
            public void Handle(ProjectileVolley m) => Got.Add(m);
            public void Handle(PlayerDied m) => Got.Add(m);
        }

        private static bool WaitFor(Func<bool> condition, int ms = 3000)
        {
            var deadline = DateTime.UtcNow.AddMilliseconds(ms);
            while (DateTime.UtcNow < deadline)
            {
                if (condition()) return true;
                Thread.Sleep(5);
            }
            return condition();
        }

        [Test]
        public void MessagesFlowBothWaysAndArriveOnPoll()
        {
            var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            var port = ((IPEndPoint)listener.LocalEndpoint).Port;
            var serverSide = Task.Run(() =>
            {
                using (var client = listener.AcceptTcpClient())
                {
                    var stream = client.GetStream();
                    var decoder = new FrameDecoder();
                    var buffer = new byte[1024];
                    Frame frame;
                    while (!decoder.TryNext(out frame)) decoder.Feed(buffer, 0, stream.Read(buffer, 0, buffer.Length));
                    var hello = (Hello)MessageCodec.DecodeClient(frame.Id, new ByteReader(frame.Payload));
                    var reply = Framing.Encode(new HelloAck { ProtocolVersion = 1, SessionId = 3, AccountId = 4, AccountName = hello.Token, ServerTimeMs = 5 });
                    var pong = Framing.Encode(new Pong { ClientTimeMs = 6, ServerTimeMs = 7 });
                    stream.Write(reply, 0, reply.Length);
                    stream.Write(pong, 0, pong.Length);
                    Thread.Sleep(200);
                }
            });

            using (var c = new GameConnection())
            {
                c.ConnectAsync("127.0.0.1", port, TimeSpan.FromSeconds(5)).Wait();
                c.Send(new Hello { ProtocolVersion = 1, BuildVersion = "t", Token = "ticket" });
                var r = new Recorder();
                Assert.IsTrue(WaitFor(() => { c.Poll(r); return r.Got.Count == 2; }));
                Assert.AreEqual("ticket", ((HelloAck)r.Got[0]).AccountName);
                Assert.IsInstanceOf<Pong>(r.Got[1]);
                serverSide.Wait();
                Assert.IsTrue(WaitFor(() => !c.IsOpen));
                StringAssert.Contains("closed", c.CloseReason);
            }
            listener.Stop();
        }

        [Test]
        public void GarbageFromTheServerClosesTheConnection()
        {
            var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            var port = ((IPEndPoint)listener.LocalEndpoint).Port;
            var serverSide = Task.Run(() =>
            {
                using (var client = listener.AcceptTcpClient())
                {
                    var junk = new byte[] { 0, 0, 0, 0, 0x34, 0x12 };  // unknown message id
                    client.GetStream().Write(junk, 0, junk.Length);
                    Thread.Sleep(500);
                }
            });
            using (var c = new GameConnection())
            {
                c.ConnectAsync("127.0.0.1", port, TimeSpan.FromSeconds(5)).Wait();
                Assert.IsTrue(WaitFor(() => !c.IsOpen));
                StringAssert.Contains("protocol error", c.CloseReason);
            }
            serverSide.Wait();
            listener.Stop();
        }

        [Test]
        public void AnUnreachableServerFailsWithinTheTimeout()
        {
            using (var c = new GameConnection())
            {
                var e = Assert.Throws<AggregateException>(() => c.ConnectAsync("127.0.0.1", 1, TimeSpan.FromSeconds(3)).Wait());
                Assert.IsTrue(e.InnerException is SocketException || e.InnerException is TimeoutException);
            }
        }

        private sealed class FakeHandler : HttpMessageHandler
        {
            public readonly List<HttpRequestMessage> Requests = new List<HttpRequestMessage>();
            public readonly List<string> Bodies = new List<string>();
            public Func<HttpRequestMessage, HttpResponseMessage> Respond;

            protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
            {
                Requests.Add(request);
                Bodies.Add(request.Content == null ? null : await request.Content.ReadAsStringAsync());
                return Respond(request);
            }
        }

        private static HttpResponseMessage Json(HttpStatusCode code, string json) =>
            new HttpResponseMessage(code) { Content = new StringContent(json, Encoding.UTF8, "application/json") };

        [Test]
        public void TheApiClientKeepsOnlyTheTokenAndSendsItAsBearer()
        {
            var handler = new FakeHandler
            {
                Respond = r => r.RequestUri.AbsolutePath.EndsWith("/sessions")
                    ? Json(HttpStatusCode.OK, "{\"token\":\"tok\",\"accountId\":1,\"name\":\"Bob\",\"expiresInSeconds\":60}")
                    : Json(HttpStatusCode.OK, "{\"ticket\":\"t1\",\"gameHost\":\"127.0.0.1\",\"gamePort\":2050,\"expiresInSeconds\":60}"),
            };
            using (var api = new ApiClient("http://example.test", handler))
            {
                api.LoginAsync("Bob", "password123").Wait();
                Assert.AreEqual("tok", api.Token);
                var join = api.JoinAsync().Result;
                Assert.AreEqual("t1", join.Ticket);
                Assert.AreEqual(2050, join.GamePort);
                Assert.AreEqual("Bearer", handler.Requests[1].Headers.Authorization.Scheme);
                Assert.AreEqual("tok", handler.Requests[1].Headers.Authorization.Parameter);
                Assert.IsNull(handler.Bodies[1]);  // the password is never sent again
            }
        }

        [Test]
        public void ApiErrorsCarryTheServersMessage()
        {
            var handler = new FakeHandler { Respond = _ => Json(HttpStatusCode.Conflict, "{\"error\":\"That name is taken.\"}") };
            using (var api = new ApiClient("http://example.test", handler))
            {
                var e = Assert.Throws<AggregateException>(() => api.RegisterAsync("Bob", "password123").Wait());
                var inner = (ApiException)e.InnerException;
                Assert.AreEqual(HttpStatusCode.Conflict, inner.Status);
                Assert.AreEqual("That name is taken.", inner.Message);
            }
        }
    }
}
