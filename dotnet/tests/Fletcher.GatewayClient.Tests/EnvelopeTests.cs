// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8a: the envelope, ported from both of its existing test files (Bucket 2).
//
// core/tests/test_envelope.cpp's eleven cases, one for one and named as there, then
// gateway-client-ts/test/envelope.test.ts's five where they add something (the TS
// "matches C++ wire format" vector, and the TS shapes the C++ ones do not spell out).
// Two cases C# adds: the order rule (attachments go out SORTED by key bytes whatever
// order the caller gave, so C# writes what C++ writes) and the zero-byte key, which C++
// refuses in both directions.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

using Xunit;

namespace Eiva.Fletcher.GatewayClient.Tests;

public sealed class EnvelopeTests
{
    // ── core/tests/test_envelope.cpp ──

    [Fact]
    public void RoundtripWithNoAttachments()
    {
        var env = new Envelope(new byte[] { 0x01, 0x02, 0x03, 0x04 });

        Envelope restored = Envelope.Deserialize(env.Serialize());

        Assert.Equal(env.Row.ToArray(), restored.Row.ToArray());
        Assert.Equal(0, restored.Count);
    }

    [Fact]
    public void RoundtripWithOneAttachment()
    {
        byte[] payload = { 0xDE, 0xAD, 0xBE, 0xEF };
        var env = new Envelope(new byte[] { 0xAA, 0xBB }, Attachments(("image", payload)));

        byte[] wire = env.Serialize();
        Envelope restored = Envelope.Deserialize(wire);

        Assert.Equal(env.Row.ToArray(), restored.Row.ToArray());
        Assert.Equal(1, restored.Count);
        Assert.True(restored.TryFind("image", out ReadOnlyMemory<byte> found));
        Assert.Equal(payload, found.ToArray());

        // The re-anchor, as C++'s case asserts it: the value does not merely compare
        // equal, it is a window INTO the parsed buffer (Deserialize borrows).
        Assert.True(System.Runtime.InteropServices.MemoryMarshal.TryGetArray(found, out ArraySegment<byte> segment));
        Assert.Same(wire, segment.Array);
    }

    [Fact]
    public void RoundtripWithMultipleAttachments()
    {
        byte[] a = { 0x01, 0x02 };
        byte[] b = { 0x03, 0x04, 0x05 };
        var env = new Envelope(new byte[] { 0xFF }, Attachments(("a", a), ("b", b), ("empty", System.Array.Empty<byte>())));

        Envelope restored = Envelope.Deserialize(env.Serialize());

        Assert.Equal(new byte[] { 0xFF }, restored.Row.ToArray());
        Assert.Equal(3, restored.Count);
        Assert.Equal(a, Find(restored, "a"));
        Assert.Equal(b, Find(restored, "b"));
        Assert.True(restored.TryFind("empty", out ReadOnlyMemory<byte> empty));
        Assert.True(empty.IsEmpty);
    }

    [Fact]
    public void RoundtripWithLargeBlob()
    {
        byte[] big = new byte[1_100_000];
        System.Array.Fill(big, (byte)0x42);
        var env = new Envelope(new byte[] { 0x00 }, Attachments(("big", big)));

        Envelope restored = Envelope.Deserialize(env.Serialize());

        Assert.Equal(new byte[] { 0x00 }, restored.Row.ToArray());
        Assert.True(restored.TryFind("big", out ReadOnlyMemory<byte> found));
        Assert.Equal(1_100_000, found.Length);
        Assert.Equal(0x42, found.Span[0]);
    }

    [Fact]
    public void EmptyRowWithAttachments()
    {
        var env = new Envelope(ReadOnlyMemory<byte>.Empty, Attachments(("data", new byte[] { 0x01 })));

        Envelope restored = Envelope.Deserialize(env.Serialize());

        Assert.True(restored.Row.IsEmpty);
        Assert.Equal(1, restored.Count);
        Assert.Equal(new byte[] { 0x01 }, Find(restored, "data"));
    }

    [Fact]
    public void CompletelyEmptyEnvelope()
    {
        var env = new Envelope(ReadOnlyMemory<byte>.Empty);

        byte[] wire = env.Serialize();
        Envelope restored = Envelope.Deserialize(wire);

        Assert.Equal(8, wire.Length);
        Assert.True(restored.Row.IsEmpty);
        Assert.Equal(0, restored.Count);
    }

    [Fact]
    public void ThrowsOnTruncatedBuffer()
    {
        Assert.Throws<InvalidDataException>(() => Envelope.Deserialize(new byte[] { 0x01, 0x02 }));
    }

    [Fact]
    public void ThrowsOnTruncatedRowData()
    {
        // row_len = 100, then only 4 bytes.
        Assert.Throws<InvalidDataException>(() => Envelope.Deserialize(Convert.FromHexString("64000000" + "01020304")));
    }

    [Fact]
    public void ThrowsOnTruncatedAttachmentKey()
    {
        // row_len = 1, row 0xFF, attach_count = 1, key_len = 100 and no key.
        Assert.Throws<InvalidDataException>(() => Envelope.Deserialize(Convert.FromHexString("01000000" + "FF" + "01000000" + "64000000")));
    }

    [Fact]
    public void ADescendingAttachmentKeyOrderDoesNotMakeDecodeQuadratic()
    {
        // The C++ case's count and shape: 200 000 fixed-width keys arriving in DESCENDING
        // order, the worst case for a sorted insert per key. The decoder sorts once, so
        // this is a timeout test by construction: a quadratic decoder would take minutes.
        const int count = 200_000;
        var wire = new List<byte>(count * 16 + 8);
        AppendU32(wire, 0);
        AppendU32(wire, count);
        for (int i = 0; i < count; ++i)
        {
            byte[] key = Encoding.ASCII.GetBytes(FixedWidthKey(count - 1 - i));
            AppendU32(wire, key.Length);
            wire.AddRange(key);
            AppendU32(wire, 0);
        }

        var watch = System.Diagnostics.Stopwatch.StartNew();
        Envelope parsed = Envelope.Deserialize(wire.ToArray());
        watch.Stop();

        Assert.Equal(count, parsed.Count);
        Assert.Equal(FixedWidthKey(0), Encoding.ASCII.GetString(parsed.KeyAt(0)));
        Assert.Equal(FixedWidthKey(count - 1), Encoding.ASCII.GetString(parsed.KeyAt(count - 1)));
        for (int i = 1; i < parsed.Count; ++i)
        {
            Assert.True(parsed.KeyAt(i - 1).SequenceCompareTo(parsed.KeyAt(i)) < 0, "not ascending at " + i);
        }

        Assert.True(parsed.TryFind(FixedWidthKey(12345), out _));
        Assert.True(watch.Elapsed < TimeSpan.FromSeconds(30), "decode took " + watch.Elapsed);
    }

    [Fact]
    public void ADuplicateAttachmentKeyOnTheWireResolvesLastWins()
    {
        var wire = new List<byte>();
        AppendU32(wire, 0);
        AppendU32(wire, 3);
        foreach ((string key, byte value) in new[] { ("k", (byte)0x11), ("a", (byte)0x22), ("k", (byte)0x33) })
        {
            AppendU32(wire, key.Length);
            wire.AddRange(Encoding.ASCII.GetBytes(key));
            AppendU32(wire, 1);
            wire.Add(value);
        }

        Envelope parsed = Envelope.Deserialize(wire.ToArray());

        Assert.Equal(2, parsed.Count);
        Assert.Equal("a", Encoding.ASCII.GetString(parsed.KeyAt(0)));
        Assert.Equal("k", Encoding.ASCII.GetString(parsed.KeyAt(1)));
        Assert.Equal(new byte[] { 0x33 }, Find(parsed, "k"));
    }

    // ── gateway-client-ts test/envelope.test.ts ──

    [Fact]
    public void MatchesTheCppWireFormat()
    {
        // row = [0x42], no attachments: row_len 1, the row, attach_count 0.
        byte[] expected = Convert.FromHexString("01000000" + "42" + "00000000");

        Assert.Equal(expected, new Envelope(new byte[] { 0x42 }).Serialize());
        Assert.Equal(new byte[] { 0x42 }, Envelope.Deserialize(expected).Row.ToArray());
    }

    [Fact]
    public void RoundTripsAnEnvelopeWithTwoAttachments()
    {
        var env = new Envelope(new byte[] { 0xDE, 0xAD }, Attachments(("image", new byte[] { 0xFF, 0xD8, 0xFF }), ("meta", new byte[] { 0x01 })));

        Envelope restored = Envelope.Deserialize(env.Serialize());

        Assert.Equal(new byte[] { 0xDE, 0xAD }, restored.Row.ToArray());
        Assert.Equal(new byte[] { 0xFF, 0xD8, 0xFF }, Find(restored, "image"));
        Assert.Equal(new byte[] { 0x01 }, Find(restored, "meta"));
    }

    // ── C# additions ──

    [Fact]
    public void AttachmentsGoOutSortedByKeyBytesWhateverTheCallersOrder()
    {
        // C++ holds attachments sorted and writes them in that order. "Z" (0x5A) sorts
        // before "a" (0x61) by bytes; "é" is 0xC3 0xA9 in UTF-8 and sorts last.
        var env = new Envelope(ReadOnlyMemory<byte>.Empty, Attachments(("é", new byte[] { 3 }), ("a", new byte[] { 2 }), ("Z", new byte[] { 1 })));

        string expected = "00000000" + "03000000" +
            "01000000" + "5A" + "01000000" + "01" +
            "01000000" + "61" + "01000000" + "02" +
            "02000000" + "C3A9" + "01000000" + "03";
        Assert.Equal(expected, Convert.ToHexString(env.Serialize()));
    }

    [Fact]
    public void AKeyGivenTwiceByTheCallerKeepsItsLastValue()
    {
        var env = new Envelope(ReadOnlyMemory<byte>.Empty, Attachments(("k", new byte[] { 1 }), ("k", new byte[] { 2 })));

        Assert.Equal(1, env.Count);
        Assert.Equal(new byte[] { 2 }, Find(env, "k"));
    }

    [Fact]
    public void AZeroByteInAKeyIsRefusedBothWays()
    {
        Assert.Throws<ArgumentException>(() => new Envelope(ReadOnlyMemory<byte>.Empty, Attachments(("a\0b", new byte[] { 1 }))));
        Assert.Throws<InvalidDataException>(() => Envelope.Deserialize(Convert.FromHexString("00000000" + "01000000" + "03000000" + "610062" + "00000000")));
    }

    [Fact]
    public void AnAttachmentCountTheBufferCannotHoldIsRefusedBeforeAllocating()
    {
        // 0xFFFFFFFF attachments announced in a 12-byte buffer.
        Assert.Throws<InvalidDataException>(() => Envelope.Deserialize(Convert.FromHexString("00000000" + "FFFFFFFF" + "00000000")));
    }

    private static IEnumerable<KeyValuePair<string, ReadOnlyMemory<byte>>> Attachments(params (string Key, byte[] Value)[] entries)
    {
        foreach ((string key, byte[] value) in entries)
        {
            yield return new KeyValuePair<string, ReadOnlyMemory<byte>>(key, value);
        }
    }

    private static byte[] Find(Envelope envelope, string key)
    {
        Assert.True(envelope.TryFind(key, out ReadOnlyMemory<byte> value), "no attachment named " + key);
        return value.ToArray();
    }

    private static void AppendU32(List<byte> wire, int value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            wire.Add((byte)((uint)value >> shift));
        }
    }

    private static string FixedWidthKey(int value) => value.ToString("D8", System.Globalization.CultureInfo.InvariantCulture);
}
