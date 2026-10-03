using System;
using System.Collections.Generic;
using System.Linq;
using NUnit.Framework;

namespace WaW.Protocol.Tests
{
    public class ProtocolTests
    {
        private static string Hex(ReadOnlySpan<byte> bytes)
        {
            var chars = new char[bytes.Length * 2];
            const string digits = "0123456789abcdef";
            for (int i = 0; i < bytes.Length; i++)
            {
                chars[i * 2] = digits[bytes[i] >> 4];
                chars[i * 2 + 1] = digits[bytes[i] & 0xF];
            }
            return new string(chars);
        }

        private static byte[] FromHex(string hex)
        {
            var bytes = new byte[hex.Length / 2];
            for (int i = 0; i < bytes.Length; i++) bytes[i] = Convert.ToByte(hex.Substring(i * 2, 2), 16);
            return bytes;
        }

        private static IMessage Decode(GoldenSamples.Sample s, byte[] payload)
        {
            var r = new ByteReader(payload);
            return s.ClientToServer ? MessageCodec.DecodeClient(s.Id, r) : MessageCodec.DecodeServer(s.Id, r);
        }

        private static string Encode(IMessage m)
        {
            var w = new ByteWriter();
            m.Write(w);
            return Hex(w.Written);
        }

        public static IEnumerable<string> SampleNames => GoldenSamples.All.Select(s => s.Name);

        [TestCaseSource(nameof(SampleNames))]
        public void EncodesToGoldenBytes(string name)
        {
            var s = GoldenSamples.All.Single(x => x.Name == name);
            var message = s.Make();
            Assert.AreEqual(s.Id, (ushort)message.Id);
            Assert.AreEqual(s.Hex, Encode(message));
        }

        [TestCaseSource(nameof(SampleNames))]
        public void DecodesGoldenBytesBackToTheSameMessage(string name)
        {
            var s = GoldenSamples.All.Single(x => x.Name == name);
            var decoded = Decode(s, FromHex(s.Hex));
            Assert.AreEqual(s.Make().GetType(), decoded.GetType());
            Assert.AreEqual(s.Hex, Encode(decoded));
        }

        [TestCaseSource(nameof(SampleNames))]
        public void RefusesTruncatedAndTrailingPayloads(string name)
        {
            var s = GoldenSamples.All.Single(x => x.Name == name);
            var bytes = FromHex(s.Hex);
            if (bytes.Length > 0)
                Assert.Throws<ProtocolException>(() => Decode(s, bytes.Take(bytes.Length - 1).ToArray()));
            Assert.Throws<ProtocolException>(() => Decode(s, bytes.Concat(new byte[] { 0 }).ToArray()));
        }

        [Test]
        public void RefusesWrongDirectionAndUnknownIds()
        {
            Assert.Throws<ProtocolException>(() => MessageCodec.DecodeServer((ushort)MessageId.Hello, new ByteReader(new byte[0])));
            Assert.Throws<ProtocolException>(() => MessageCodec.DecodeClient((ushort)MessageId.Snapshot, new ByteReader(new byte[0])));
            Assert.Throws<ProtocolException>(() => MessageCodec.DecodeClient(60000, new ByteReader(new byte[0])));
        }

        [Test]
        public void RefusesInvalidBoolAndEnumValues()
        {
            Assert.Throws<ProtocolException>(() =>
                MessageCodec.DecodeServer((ushort)MessageId.Failure, new ByteReader(new byte[] { 1, 0, 2, 0, 0 })));
            Assert.Throws<ProtocolException>(() =>
                MessageCodec.DecodeServer((ushort)MessageId.Failure, new ByteReader(new byte[] { 0xFF, 0x7F, 1, 0, 0 })));
        }

        [Test]
        public void RefusesAListCountLargerThanThePayload()
        {
            Assert.Throws<ProtocolException>(() =>
                MessageCodec.DecodeClient((ushort)MessageId.MoveInput, new ByteReader(new byte[] { 0xFF, 0xFF })));
        }

        [Test]
        public void RandomBytesOnlyEverThrowProtocolException()
        {
            var rng = new Random(1234);
            for (int i = 0; i < 20000; i++)
            {
                var payload = new byte[rng.Next(0, 65)];
                rng.NextBytes(payload);
                try
                {
                    MessageCodec.DecodeServer((ushort)(101 + i % 12), new ByteReader(payload));
                }
                catch (ProtocolException)
                {
                }
            }
        }

        [Test]
        public void FramesRoundTripByteByByte()
        {
            var a = Framing.Encode(new Ping { ClientTimeMs = 123456 });
            var b = Framing.Encode(new ChatSend { Text = "hello é" });
            Assert.AreEqual(Framing.HeaderBytes + 4, a.Length);
            var stream = a.Concat(b).ToArray();

            var decoder = new FrameDecoder();
            var frames = new List<Frame>();
            foreach (var t in stream)
            {
                decoder.Feed(new[] { t }, 0, 1);
                while (decoder.TryNext(out var f)) frames.Add(f);
            }
            Assert.AreEqual(2, frames.Count);
            var ping = (Ping)MessageCodec.DecodeClient(frames[0].Id, new ByteReader(frames[0].Payload));
            Assert.AreEqual(123456u, ping.ClientTimeMs);
            var chat = (ChatSend)MessageCodec.DecodeClient(frames[1].Id, new ByteReader(frames[1].Payload));
            Assert.AreEqual("hello é", chat.Text);
            Assert.AreEqual(0, decoder.Buffered);
        }

        [Test]
        public void AnOversizedFrameIsRefused()
        {
            var decoder = new FrameDecoder(1024);
            decoder.Feed(new byte[] { 0x01, 0x04, 0, 0, 2, 0 }, 0, 6);
            Assert.Throws<ProtocolException>(() => decoder.TryNext(out _));
        }

        private sealed class RecordingHandler : IServerMessageHandler
        {
            public readonly List<string> Seen = new List<string>();
            public void Handle(HelloAck message) => Seen.Add(nameof(HelloAck));
            public void Handle(Failure message) => Seen.Add(nameof(Failure));
            public void Handle(Pong message) => Seen.Add(nameof(Pong));
            public void Handle(WorldInfo message) => Seen.Add(nameof(WorldInfo));
            public void Handle(TileData message) => Seen.Add(nameof(TileData));
            public void Handle(PlayerSpawned message) => Seen.Add(nameof(PlayerSpawned));
            public void Handle(Snapshot message) => Seen.Add(nameof(Snapshot));
            public void Handle(ChatMessage message) => Seen.Add(nameof(ChatMessage));
            public void Handle(Notification message) => Seen.Add(nameof(Notification));
            public void Handle(PlayerStats message) => Seen.Add(nameof(PlayerStats));
            public void Handle(Inventory message) => Seen.Add(nameof(Inventory));
            public void Handle(ProjectileVolley message) => Seen.Add(nameof(ProjectileVolley));
            public void Handle(PlayerDied message) => Seen.Add(nameof(PlayerDied));
        }

        [Test]
        public void DispatcherRoutesEveryServerMessage()
        {
            var handler = new RecordingHandler();
            foreach (var s in GoldenSamples.All.Where(x => !x.ClientToServer))
                ServerMessageDispatcher.Dispatch(s.Make(), handler);
            CollectionAssert.AreEqual(GoldenSamples.All.Where(x => !x.ClientToServer).Select(x => x.Name).ToList(), handler.Seen);
        }
    }
}
