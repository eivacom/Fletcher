// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The envelope a gateway data frame carries: one encoded row and its attachments.
//
// It lives here and nowhere else (Q15): the native path hands rows and attachments to
// the shim separately and never sees this framing. The layout is core/include/fletcher/
// core/envelope.hpp's, little-endian:
//
//   [ROW_LEN : u32][ROW : ROW_LEN][ATTACH_COUNT : u32]
//   ATTACH_COUNT x { [KEY_LEN : u32][KEY : KEY_LEN][BLOB_LEN : u32][BLOB : BLOB_LEN] }
//
// Two behaviours are C++'s and are kept on purpose. The attachments are held SORTED by
// their key bytes (an unsigned byte compare, as C++'s Attachments does), so C# writes
// the order C++ writes whatever order the caller gave. And a key that appears twice -
// on the wire or in the caller's sequence - resolves LAST-WINS, as C++'s decoder does.
// The decoder sorts once, so a hostile frame with its keys in descending order costs
// O(k log k), not the O(k^2) a sorted insert per key would (core/tests/test_envelope.cpp,
// ADescendingAttachmentKeyOrderDoesNotMakeDecodeQuadratic).
using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>One encoded row and its attachments, as a gateway data frame carries them.</summary>
/// <remarks>
/// Immutable. An envelope read with <see cref="Deserialize"/> BORROWS the buffer it was
/// read from: <see cref="Row"/> and every attachment value are windows onto it, so that
/// buffer must not be reused while the envelope is in use.
/// </remarks>
public sealed class Envelope
{
    private readonly byte[][] _keys;
    private readonly ReadOnlyMemory<byte>[] _values;

    /// <summary>An envelope with a row and no attachments.</summary>
    /// <param name="row">The encoded row.</param>
    public Envelope(ReadOnlyMemory<byte> row)
    {
        Row = row;
        _keys = Array.Empty<byte[]>();
        _values = Array.Empty<ReadOnlyMemory<byte>>();
    }

    /// <summary>An envelope with a row and attachments.</summary>
    /// <param name="row">The encoded row.</param>
    /// <param name="attachments">
    /// The attachments, in any order. Keys are written as UTF-8. A key given twice keeps
    /// its LAST value.
    /// </param>
    /// <exception cref="ArgumentNullException"><paramref name="attachments"/> or a key is null.</exception>
    /// <exception cref="ArgumentException">A key contains a zero character, which C++ refuses.</exception>
    public Envelope(ReadOnlyMemory<byte> row, IEnumerable<KeyValuePair<string, ReadOnlyMemory<byte>>> attachments)
    {
        ArgumentNullException.ThrowIfNull(attachments);
        Row = row;

        var entries = new List<(byte[] Key, ReadOnlyMemory<byte> Value, int Arrival)>();
        foreach (KeyValuePair<string, ReadOnlyMemory<byte>> attachment in attachments)
        {
            ArgumentNullException.ThrowIfNull(attachment.Key, nameof(attachments));
            if (attachment.Key.Contains('\0', StringComparison.Ordinal))
            {
                throw new ArgumentException("An attachment key must not contain a zero character; C++ refuses it.", nameof(attachments));
            }

            entries.Add((Encoding.UTF8.GetBytes(attachment.Key), attachment.Value, entries.Count));
        }

        (_keys, _values) = SortLastWins(entries);
    }

    private Envelope(ReadOnlyMemory<byte> row, byte[][] keys, ReadOnlyMemory<byte>[] values)
    {
        Row = row;
        _keys = keys;
        _values = values;
    }

    /// <summary>The encoded row.</summary>
    public ReadOnlyMemory<byte> Row { get; }

    /// <summary>The number of attachments.</summary>
    public int Count => _keys.Length;

    /// <summary>The key of the attachment at <paramref name="index"/>, as UTF-8 bytes.</summary>
    /// <remarks>Attachments are ordered by their key bytes, ascending.</remarks>
    public ReadOnlySpan<byte> KeyAt(int index) => _keys[index];

    /// <summary>The value of the attachment at <paramref name="index"/>.</summary>
    public ReadOnlyMemory<byte> ValueAt(int index) => _values[index];

    /// <summary>Look an attachment up by its key bytes.</summary>
    public bool TryFind(ReadOnlySpan<byte> key, out ReadOnlyMemory<byte> value)
    {
        int lo = 0;
        int hi = _keys.Length - 1;
        while (lo <= hi)
        {
            int mid = lo + ((hi - lo) / 2);
            int order = _keys[mid].AsSpan().SequenceCompareTo(key);
            if (order == 0)
            {
                value = _values[mid];
                return true;
            }

            if (order < 0)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid - 1;
            }
        }

        value = default;
        return false;
    }

    /// <summary>Look an attachment up by its key, encoded as UTF-8.</summary>
    /// <exception cref="ArgumentNullException"><paramref name="key"/> is null.</exception>
    public bool TryFind(string key, out ReadOnlyMemory<byte> value)
    {
        ArgumentNullException.ThrowIfNull(key);
        return TryFind(Encoding.UTF8.GetBytes(key), out value);
    }

    /// <summary>The number of bytes <see cref="Serialize"/> writes.</summary>
    public int SerializedLength
    {
        get
        {
            long total = 4L + Row.Length + 4;
            for (int i = 0; i < _keys.Length; ++i)
            {
                total += 4L + _keys[i].Length + 4 + _values[i].Length;
            }

            if (total > int.MaxValue)
            {
                throw new InvalidOperationException("The envelope is larger than one buffer can hold (2 GiB).");
            }

            return (int)total;
        }
    }

    /// <summary>The envelope's wire bytes.</summary>
    public byte[] Serialize()
    {
        byte[] bytes = new byte[SerializedLength];
        WriteTo(bytes);
        return bytes;
    }

    /// <summary>Write the wire bytes into <paramref name="destination"/>, exactly <see cref="SerializedLength"/> of them.</summary>
    internal void WriteTo(Span<byte> destination)
    {
        int pos = 0;
        WriteU32(destination, ref pos, Row.Length);
        Row.Span.CopyTo(destination[pos..]);
        pos += Row.Length;

        WriteU32(destination, ref pos, _keys.Length);
        for (int i = 0; i < _keys.Length; ++i)
        {
            WriteU32(destination, ref pos, _keys[i].Length);
            _keys[i].CopyTo(destination[pos..]);
            pos += _keys[i].Length;

            WriteU32(destination, ref pos, _values[i].Length);
            _values[i].Span.CopyTo(destination[pos..]);
            pos += _values[i].Length;
        }
    }

    /// <summary>Read an envelope from its wire bytes.</summary>
    /// <param name="data">The wire bytes. BORROWED: the envelope's row and values are windows onto it.</param>
    /// <exception cref="InvalidDataException">
    /// The bytes are not a whole envelope, or an attachment key contains a zero byte.
    /// </exception>
    /// <remarks>
    /// Trailing bytes after the last attachment are ignored, as C++'s decoder ignores
    /// them: a data frame's envelope runs to the end of the frame.
    /// </remarks>
    public static Envelope Deserialize(ReadOnlyMemory<byte> data)
    {
        ReadOnlySpan<byte> span = data.Span;
        if (span.Length < 8)
        {
            throw new InvalidDataException("Envelope: buffer too small (" + span.Length + " bytes; a header is 8)");
        }

        int pos = 0;
        int rowLength = ReadLength(span, ref pos, "row");
        ReadOnlyMemory<byte> row = data.Slice(pos, rowLength);
        pos += rowLength;

        uint count = ReadU32(span, ref pos, "attachment count");

        // Every attachment needs at least its two length prefixes, so a count the
        // remaining bytes cannot hold is refused before anything is allocated for it.
        if (count > (uint)(span.Length - pos) / 8)
        {
            throw new InvalidDataException("Envelope: attachment count " + count + " exceeds the remaining buffer");
        }

        var entries = new List<(byte[] Key, ReadOnlyMemory<byte> Value, int Arrival)>((int)count);
        for (int i = 0; i < (int)count; ++i)
        {
            int keyLength = ReadLength(span, ref pos, "attachment key");
            byte[] key = span.Slice(pos, keyLength).ToArray();
            pos += keyLength;
            if (Array.IndexOf(key, (byte)0) >= 0)
            {
                throw new InvalidDataException("Envelope: attachment key contains a zero byte");
            }

            int valueLength = ReadLength(span, ref pos, "attachment value");
            ReadOnlyMemory<byte> value = data.Slice(pos, valueLength);
            pos += valueLength;

            entries.Add((key, value, i));
        }

        (byte[][] keys, ReadOnlyMemory<byte>[] values) = SortLastWins(entries);
        return new Envelope(row, keys, values);
    }

    // Sort once by key bytes, ties by arrival, then keep the LAST of every run of
    // equal keys: the stable equivalent of inserting one by one with replacement.
    private static (byte[][] Keys, ReadOnlyMemory<byte>[] Values) SortLastWins(
        List<(byte[] Key, ReadOnlyMemory<byte> Value, int Arrival)> entries)
    {
        entries.Sort(static (a, b) =>
        {
            int order = a.Key.AsSpan().SequenceCompareTo(b.Key);
            return order != 0 ? order : a.Arrival.CompareTo(b.Arrival);
        });

        var keys = new List<byte[]>(entries.Count);
        var values = new List<ReadOnlyMemory<byte>>(entries.Count);
        for (int i = 0; i < entries.Count; ++i)
        {
            bool supersededByNext = i + 1 < entries.Count && entries[i + 1].Key.AsSpan().SequenceEqual(entries[i].Key);
            if (supersededByNext)
            {
                continue;
            }

            keys.Add(entries[i].Key);
            values.Add(entries[i].Value);
        }

        return (keys.ToArray(), values.ToArray());
    }

    private static void WriteU32(Span<byte> destination, ref int pos, int value)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(destination[pos..], (uint)value);
        pos += 4;
    }

    private static uint ReadU32(ReadOnlySpan<byte> span, ref int pos, string what)
    {
        if (span.Length - pos < 4)
        {
            throw new InvalidDataException("Envelope: unexpected end of buffer reading the " + what);
        }

        uint value = BinaryPrimitives.ReadUInt32LittleEndian(span[pos..]);
        pos += 4;
        return value;
    }

    // A length prefix, checked against what is left: the run it announces must be there.
    private static int ReadLength(ReadOnlySpan<byte> span, ref int pos, string what)
    {
        uint length = ReadU32(span, ref pos, what + " length");
        if (length > (uint)(span.Length - pos))
        {
            throw new InvalidDataException("Envelope: " + what + " truncated (" + length + " bytes announced, " +
                (span.Length - pos) + " left)");
        }

        return (int)length;
    }
}
