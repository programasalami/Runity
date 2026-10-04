using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using Runity.Protocol;

namespace Runity.Net
{
    /// <summary>The TCP connection to the game server. A reader thread splits frames and decodes server messages into a queue; a
    /// writer thread sends queued frames. The game thread calls Poll() once per frame to handle what arrived, so game state is
    /// only ever touched on that thread. (A WebSocket transport for a WebGL build would replace the socket part only.)</summary>
    public sealed class GameConnection : IDisposable
    {
        private readonly ConcurrentQueue<IMessage> _inbound = new ConcurrentQueue<IMessage>();
        private readonly BlockingCollection<byte[]> _outbound = new BlockingCollection<byte[]>(new ConcurrentQueue<byte[]>());
        private TcpClient _tcp;
        private NetworkStream _stream;
        private Thread _reader;
        private Thread _writer;
        private volatile bool _open;
        private volatile string _closeReason;

        public bool IsOpen => _open;
        /// <summary>Why the connection closed (null while open).</summary>
        public string CloseReason => _closeReason;
        public long BytesReceived { get; private set; }
        public long BytesSent { get; private set; }

        public async Task ConnectAsync(string host, int port, TimeSpan timeout, CancellationToken cancel = default)
        {
            if (_tcp != null) throw new InvalidOperationException("already connected");
            _tcp = new TcpClient { NoDelay = true };
            var connect = _tcp.ConnectAsync(host, port);
            var finished = await Task.WhenAny(connect, Task.Delay(timeout, cancel)).ConfigureAwait(false);
            if (finished != connect)
            {
                _tcp.Dispose();
                throw new TimeoutException($"could not reach the game server at {host}:{port}");
            }
            await connect.ConfigureAwait(false);  // rethrows a connection error
            _stream = _tcp.GetStream();
            _open = true;
            _reader = new Thread(ReadLoop) { IsBackground = true, Name = "Runity net read" };
            _writer = new Thread(WriteLoop) { IsBackground = true, Name = "Runity net write" };
            _reader.Start();
            _writer.Start();
        }

        /// <summary>Queues a message (any thread). Ignored once the connection is closed.</summary>
        public void Send(IMessage message)
        {
            if (!_open) return;
            try
            {
                _outbound.Add(Framing.Encode(message));
            }
            catch (InvalidOperationException)
            {
                // Closed concurrently.
            }
        }

        /// <summary>Hands every received message to the handler, in arrival order. Call on the game thread.</summary>
        public int Poll(IServerMessageHandler handler, int maxMessages = int.MaxValue)
        {
            var n = 0;
            while (n < maxMessages && _inbound.TryDequeue(out var message))
            {
                ServerMessageDispatcher.Dispatch(message, handler);
                n++;
            }
            return n;
        }

        private void ReadLoop()
        {
            var decoder = new FrameDecoder();
            var buffer = new byte[64 * 1024];
            try
            {
                while (_open)
                {
                    var n = _stream.Read(buffer, 0, buffer.Length);
                    if (n == 0)
                    {
                        Close("the server closed the connection");
                        return;
                    }
                    BytesReceived += n;
                    decoder.Feed(buffer, 0, n);
                    while (decoder.TryNext(out var frame))
                        _inbound.Enqueue(MessageCodec.DecodeServer(frame.Id, new ByteReader(frame.Payload)));
                }
            }
            catch (ProtocolException e)
            {
                Close("protocol error: " + e.Message);
            }
            catch (Exception e) when (e is IOException || e is SocketException || e is ObjectDisposedException)
            {
                Close("connection lost: " + e.Message);
            }
        }

        private void WriteLoop()
        {
            try
            {
                foreach (var frame in _outbound.GetConsumingEnumerable())
                {
                    _stream.Write(frame, 0, frame.Length);
                    BytesSent += frame.Length;
                }
            }
            catch (Exception e) when (e is IOException || e is SocketException || e is ObjectDisposedException)
            {
                Close("connection lost: " + e.Message);
            }
        }

        public void Close(string reason)
        {
            if (!_open) return;
            _closeReason = reason;
            _open = false;
            _outbound.CompleteAdding();
            try
            {
                _tcp?.Close();
            }
            catch (SocketException)
            {
            }
        }

        public void Dispose()
        {
            Close("closed by the client");
            if (_reader != null && _reader != Thread.CurrentThread) _reader.Join(1000);
            if (_writer != null && _writer != Thread.CurrentThread) _writer.Join(1000);
            _outbound.Dispose();
            _tcp?.Dispose();
        }
    }
}
