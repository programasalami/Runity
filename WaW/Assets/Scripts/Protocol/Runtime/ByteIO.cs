using System;
using System.Buffers.Binary;
using System.Text;

namespace WaW.Protocol
{
    /// <summary>Thrown for any malformed input. The connection that produced it must be closed.</summary>
    public sealed class ProtocolException : Exception
    {
        public ProtocolException(string message) : base(message) { }
    }

    /// <summary>A protocol message (generated). Write emits the payload only; framing adds the header.</summary>
    public interface IMessage
    {
        MessageId Id { get; }
        void Write(ByteWriter w);
    }

    /// <summary>Little-endian writer into a growable buffer. Wire rules: Protocol/schema/protocol.toml.</summary>
    public sealed class ByteWriter
    {
        private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);
        private byte[] _buffer;
        private int _length;

        public ByteWriter(int capacity = 256)
        {
            _buffer = new byte[Math.Max(16, capacity)];
        }

        public int Length => _length;
        public ReadOnlySpan<byte> Written => new ReadOnlySpan<byte>(_buffer, 0, _length);
        public byte[] ToArray() => Written.ToArray();
        public void Clear() => _length = 0;

        private Span<byte> Reserve(int count)
        {
            if (_length + count > _buffer.Length)
            {
                int size = _buffer.Length;
                while (size < _length + count) size *= 2;
                Array.Resize(ref _buffer, size);
            }
            var span = new Span<byte>(_buffer, _length, count);
            _length += count;
            return span;
        }

        public void WriteU8(byte v) => Reserve(1)[0] = v;
        public void WriteI8(sbyte v) => Reserve(1)[0] = unchecked((byte)v);
        public void WriteBool(bool v) => Reserve(1)[0] = v ? (byte)1 : (byte)0;
        public void WriteU16(ushort v) => BinaryPrimitives.WriteUInt16LittleEndian(Reserve(2), v);
        public void WriteI16(short v) => BinaryPrimitives.WriteInt16LittleEndian(Reserve(2), v);
        public void WriteU32(uint v) => BinaryPrimitives.WriteUInt32LittleEndian(Reserve(4), v);
        public void WriteI32(int v) => BinaryPrimitives.WriteInt32LittleEndian(Reserve(4), v);
        public void WriteU64(ulong v) => BinaryPrimitives.WriteUInt64LittleEndian(Reserve(8), v);
        public void WriteI64(long v) => BinaryPrimitives.WriteInt64LittleEndian(Reserve(8), v);
        public void WriteF32(float v) => WriteU32((uint)BitConverter.SingleToInt32Bits(v));
        public void WriteF64(double v) => WriteU64((ulong)BitConverter.DoubleToInt64Bits(v));

        public void WriteString(string v)
        {
            if (v == null) throw new ProtocolException("string field is null");
            int count = Utf8.GetByteCount(v);
            if (count > ushort.MaxValue) throw new ProtocolException("string longer than 65535 bytes");
            WriteU16((ushort)count);
            Utf8.GetBytes(v, Reserve(count));
        }

        public void WriteListCount(int count)
        {
            if (count > ushort.MaxValue) throw new ProtocolException("list longer than 65535 elements");
            WriteU16((ushort)count);
        }

        public void PatchU32(int offset, uint v) => BinaryPrimitives.WriteUInt32LittleEndian(new Span<byte>(_buffer, offset, 4), v);
    }

    /// <summary>Little-endian reader over a byte range. Every read past the end throws ProtocolException.</summary>
    public sealed class ByteReader
    {
        private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);
        private readonly byte[] _data;
        private readonly int _end;
        private int _pos;

        public ByteReader(byte[] data) : this(data, 0, data.Length) { }

        public ByteReader(byte[] data, int offset, int count)
        {
            if (offset < 0 || count < 0 || offset + count > data.Length) throw new ArgumentOutOfRangeException(nameof(count));
            _data = data;
            _pos = offset;
            _end = offset + count;
        }

        public int Remaining => _end - _pos;

        private ReadOnlySpan<byte> Take(int count)
        {
            if (count > Remaining) throw new ProtocolException("unexpected end of message");
            var span = new ReadOnlySpan<byte>(_data, _pos, count);
            _pos += count;
            return span;
        }

        public byte ReadU8() => Take(1)[0];
        public sbyte ReadI8() => unchecked((sbyte)Take(1)[0]);
        public bool ReadBool()
        {
            byte b = Take(1)[0];
            if (b > 1) throw new ProtocolException("invalid bool value " + b);
            return b == 1;
        }
        public ushort ReadU16() => BinaryPrimitives.ReadUInt16LittleEndian(Take(2));
        public short ReadI16() => BinaryPrimitives.ReadInt16LittleEndian(Take(2));
        public uint ReadU32() => BinaryPrimitives.ReadUInt32LittleEndian(Take(4));
        public int ReadI32() => BinaryPrimitives.ReadInt32LittleEndian(Take(4));
        public ulong ReadU64() => BinaryPrimitives.ReadUInt64LittleEndian(Take(8));
        public long ReadI64() => BinaryPrimitives.ReadInt64LittleEndian(Take(8));
        public float ReadF32() => BitConverter.Int32BitsToSingle((int)ReadU32());
        public double ReadF64() => BitConverter.Int64BitsToDouble((long)ReadU64());

        public string ReadString()
        {
            int count = ReadU16();
            var bytes = Take(count);
            try
            {
                return Utf8.GetString(bytes);
            }
            catch (DecoderFallbackException)
            {
                throw new ProtocolException("string is not valid UTF-8");
            }
        }

        public int ReadListCount()
        {
            int count = ReadU16();
            // Every element is at least one byte: a larger count is malformed (and must not drive an allocation).
            if (count > Remaining) throw new ProtocolException("list count larger than the message");
            return count;
        }
    }
}
