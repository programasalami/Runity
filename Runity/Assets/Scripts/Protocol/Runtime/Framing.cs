using System;
using System.Buffers.Binary;

namespace Runity.Protocol
{
    /// <summary>A received frame: message id + payload bytes.</summary>
    public readonly struct Frame
    {
        public readonly ushort Id;
        public readonly byte[] Payload;

        public Frame(ushort id, byte[] payload)
        {
            Id = id;
            Payload = payload;
        }
    }

    /// <summary>Frame = [u32 LE payload length][u16 LE message id][payload]; the length counts the payload only.</summary>
    public static class Framing
    {
        public const int HeaderBytes = 6;

        /// <summary>Encodes one message as a complete frame.</summary>
        public static byte[] Encode(IMessage message)
        {
            var w = new ByteWriter(64);
            w.WriteU32(0);
            w.WriteU16((ushort)message.Id);
            message.Write(w);
            int payload = w.Length - HeaderBytes;
            if (payload > ProtocolInfo.MaxPayloadBytes) throw new ProtocolException("message too large: " + message.Id);
            w.PatchU32(0, (uint)payload);
            return w.ToArray();
        }
    }

    /// <summary>Splits a byte stream into frames. Feed received bytes, then call TryNext until it returns false.</summary>
    public sealed class FrameDecoder
    {
        private readonly int _maxPayload;
        private byte[] _buffer = new byte[4096];
        private int _start;
        private int _end;

        public FrameDecoder(int maxPayload = ProtocolInfo.MaxPayloadBytes)
        {
            _maxPayload = maxPayload;
        }

        public int Buffered => _end - _start;

        public void Feed(byte[] data, int offset, int count)
        {
            if (_end + count > _buffer.Length)
            {
                // Compact first, then grow if still needed.
                Buffer.BlockCopy(_buffer, _start, _buffer, 0, _end - _start);
                _end -= _start;
                _start = 0;
                if (_end + count > _buffer.Length)
                {
                    int size = _buffer.Length;
                    while (size < _end + count) size *= 2;
                    Array.Resize(ref _buffer, size);
                }
            }
            Buffer.BlockCopy(data, offset, _buffer, _end, count);
            _end += count;
        }

        /// <summary>Returns the next complete frame. Throws ProtocolException if the stream declares an oversized frame.</summary>
        public bool TryNext(out Frame frame)
        {
            frame = default;
            if (Buffered < Framing.HeaderBytes) return false;
            uint length = BinaryPrimitives.ReadUInt32LittleEndian(new ReadOnlySpan<byte>(_buffer, _start, 4));
            if (length > (uint)_maxPayload) throw new ProtocolException("frame length " + length + " exceeds the limit");
            ushort id = BinaryPrimitives.ReadUInt16LittleEndian(new ReadOnlySpan<byte>(_buffer, _start + 4, 2));
            if (Buffered < Framing.HeaderBytes + (int)length) return false;
            var payload = new byte[length];
            Buffer.BlockCopy(_buffer, _start + Framing.HeaderBytes, payload, 0, (int)length);
            _start += Framing.HeaderBytes + (int)length;
            if (_start == _end) _start = _end = 0;
            frame = new Frame(id, payload);
            return true;
        }
    }
}
