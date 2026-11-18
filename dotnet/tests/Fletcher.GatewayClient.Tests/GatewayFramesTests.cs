// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8a: the binary data frames (Bucket 2).
//
// gateway/tests/test_publish_frame.cpp's nine cases, one for one and named as there:
// they test the gateway's PUBLISH parser, and the C# parser is the one a test gateway
// will use in BIND-8b, so it must refuse exactly what the real one refuses. Then
// gateway-client-ts/test/ws-protocol.test.ts's three binary cases (the PUBLISH builder,
// the MESSAGE parser and its short-frame refusal), and each builder against its parser.
using System;
using System.IO;
using System.Text;

using Xunit;

namespace Eiva.Fletcher.GatewayClient.Tests;

public sealed class GatewayFramesTests
{
    // ── gateway/tests/test_publish_frame.cpp ──

    [Fact]
    public void ExtractsTopicAndEnvelope()
    {
        (string topic, ReadOnlyMemory<byte> envelope) = GatewayFrames.ParsePublish(Frame("0500", "hello", "DEADBEEF"));

        Assert.Equal("hello", topic);
        Assert.Equal("DEADBEEF", Convert.ToHexString(envelope.Span));
    }

    [Fact]
    public void EmptyEnvelopeIsAllowed()
    {
        (string topic, ReadOnlyMemory<byte> envelope) = GatewayFrames.ParsePublish(Frame("0300", "a/b", string.Empty));

        Assert.Equal("a/b", topic);
        Assert.True(envelope.IsEmpty);
    }

    [Fact]
    public void EmptyTopicIsAllowed()
    {
        (string topic, ReadOnlyMemory<byte> envelope) = GatewayFrames.ParsePublish(Frame("0000", string.Empty, "4243"));

        Assert.Equal(string.Empty, topic);
        Assert.Equal("4243", Convert.ToHexString(envelope.Span));
    }

    [Fact]
    public void ReadsTopicLengthAsLittleEndian()
    {
        (string topic, ReadOnlyMemory<byte> envelope) = GatewayFrames.ParsePublish(Frame("0100", "x", "AA"));

        Assert.Equal("x", topic);
        Assert.Equal("AA", Convert.ToHexString(envelope.Span));
    }

    [Fact]
    public void ThrowsOnEmptyFrame()
    {
        Assert.Throws<InvalidDataException>(() => GatewayFrames.ParsePublish(ReadOnlyMemory<byte>.Empty));
    }

    [Fact]
    public void ThrowsWhenTopicLenFieldIsTruncated()
    {
        Assert.Throws<InvalidDataException>(() => GatewayFrames.ParsePublish(new byte[] { 0x05 }));
    }

    [Fact]
    public void ThrowsWhenTopicRunsOffEndOfFrame()
    {
        Assert.Throws<InvalidDataException>(() => GatewayFrames.ParsePublish(Frame("0A00", "abc", string.Empty)));
    }

    [Fact]
    public void ThrowsWhenTopicLenIsMaxAndFrameTooSmall()
    {
        Assert.Throws<InvalidDataException>(() => GatewayFrames.ParsePublish(Frame("FFFF", "a", string.Empty)));
    }

    [Fact]
    public void TopicLenExactlyFillingFrameIsAccepted()
    {
        (string topic, ReadOnlyMemory<byte> envelope) = GatewayFrames.ParsePublish(Frame("0300", "abc", string.Empty));

        Assert.Equal("abc", topic);
        Assert.True(envelope.IsEmpty);
    }

    // ── gateway-client-ts test/ws-protocol.test.ts, the binary cases ──

    [Fact]
    public void BuildPublishWritesTopicLengthTopicThenEnvelope()
    {
        var envelope = new Envelope(new byte[] { 0xAA, 0xBB });

        byte[] frame = GatewayFrames.BuildPublish("t", envelope);

        Assert.Equal("0100" + "74" + Convert.ToHexString(envelope.Serialize()), Convert.ToHexString(frame));
    }

    [Fact]
    public void ParseMessageReadsTheSubscriptionIdThenTheEnvelope()
    {
        (ulong id, ReadOnlyMemory<byte> envelope) = GatewayFrames.ParseMessage(Convert.FromHexString("2A00000000000000" + "CAFE00"));

        Assert.Equal(42ul, id);
        Assert.Equal("CAFE00", Convert.ToHexString(envelope.Span));
    }

    [Fact]
    public void ParseMessageThrowsOnAFrameShorterThanEightBytes()
    {
        InvalidDataException e = Assert.Throws<InvalidDataException>(() => GatewayFrames.ParseMessage(new byte[7]));
        Assert.Contains("too short", e.Message, StringComparison.Ordinal);
    }

    // ── C# additions ──

    [Fact]
    public void ATopicIsCountedInUtf8BytesAndTheLimitIsTheU16()
    {
        string atLimit = new('x', GatewayFrames.MaxTopicBytes);
        string overByOneMultiByte = new string('x', GatewayFrames.MaxTopicBytes - 1) + "é";
        var envelope = new Envelope(ReadOnlyMemory<byte>.Empty);

        Assert.Equal(atLimit, GatewayFrames.ParsePublish(GatewayFrames.BuildPublish(atLimit, envelope)).Topic);
        Assert.Throws<ArgumentException>(() => GatewayFrames.BuildPublish(overByOneMultiByte, envelope));
    }

    [Fact]
    public void EachBuilderIsReadBackByItsParser()
    {
        var envelope = new Envelope(new byte[] { 1, 2, 3 }, new[] { new System.Collections.Generic.KeyValuePair<string, ReadOnlyMemory<byte>>("k", new byte[] { 9 }) });

        (string topic, ReadOnlyMemory<byte> published) = GatewayFrames.ParsePublish(GatewayFrames.BuildPublish("fleet/ship-7/heading", envelope));
        (ulong id, ReadOnlyMemory<byte> delivered) = GatewayFrames.ParseMessage(GatewayFrames.BuildMessage(ulong.MaxValue, envelope));

        Assert.Equal("fleet/ship-7/heading", topic);
        Assert.Equal(envelope.Serialize(), published.ToArray());
        Assert.Equal(ulong.MaxValue, id);
        Assert.Equal(envelope.Serialize(), delivered.ToArray());
    }

    private static byte[] Frame(string topicLengthHex, string topic, string envelopeHex) =>
        Convert.FromHexString(topicLengthHex + Convert.ToHexString(Encoding.UTF8.GetBytes(topic)) + envelopeHex);
}
