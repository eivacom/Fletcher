// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The gateway's two binary frames, little-endian:
//
//   PUBLISH (client -> gateway)  [TOPIC_LEN : u16][TOPIC : TOPIC_LEN, UTF-8][ENVELOPE : rest]
//   MESSAGE (gateway -> client)  [SUB_ID : u64][ENVELOPE : rest]
//
// gateway/src/publish_frame.cpp parses PUBLISH and ws_session.cpp writes MESSAGE; they
// are the authority. Each frame has both a builder and a parser here: the client needs
// one of each, and the other two are what a test gateway needs to stand in for the real
// one (BIND-8b). The parser's refusals are publish_frame.cpp's, case for case
// (gateway/tests/test_publish_frame.cpp is ported in GatewayFramesTests).
using System;
using System.Buffers.Binary;
using System.IO;
using System.Text;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>Builders and parsers for the gateway's binary data frames.</summary>
internal static class GatewayFrames
{
    /// <summary>The longest topic a PUBLISH frame can name, in UTF-8 bytes.</summary>
    public static readonly int MaxTopicBytes = ushort.MaxValue;

    /// <summary>A PUBLISH frame: <paramref name="envelope"/> to <paramref name="topic"/>.</summary>
    /// <exception cref="ArgumentException">The topic is longer than <see cref="MaxTopicBytes"/> UTF-8 bytes.</exception>
    public static byte[] BuildPublish(string topic, Envelope envelope)
    {
        ArgumentNullException.ThrowIfNull(topic);
        ArgumentNullException.ThrowIfNull(envelope);

        int topicBytes = Encoding.UTF8.GetByteCount(topic);
        if (topicBytes > MaxTopicBytes)
        {
            throw new ArgumentException(
                "Topic name too long: " + topicBytes + " bytes (max " + MaxTopicBytes + ")", nameof(topic));
        }

        byte[] frame = new byte[2 + topicBytes + envelope.SerializedLength];
        BinaryPrimitives.WriteUInt16LittleEndian(frame, (ushort)topicBytes);
        Encoding.UTF8.GetBytes(topic, frame.AsSpan(2, topicBytes));
        envelope.WriteTo(frame.AsSpan(2 + topicBytes));
        return frame;
    }

    /// <summary>Split a PUBLISH frame into its topic and its envelope bytes.</summary>
    /// <exception cref="InvalidDataException">The frame is too short for what it announces.</exception>
    public static (string Topic, ReadOnlyMemory<byte> Envelope) ParsePublish(ReadOnlyMemory<byte> frame)
    {
        ReadOnlySpan<byte> span = frame.Span;
        if (span.Length < 2)
        {
            throw new InvalidDataException("publish: frame too short for TOPIC_LEN");
        }

        int topicLength = BinaryPrimitives.ReadUInt16LittleEndian(span);
        if (2 + topicLength > span.Length)
        {
            throw new InvalidDataException("publish: truncated topic");
        }

        string topic = Encoding.UTF8.GetString(span.Slice(2, topicLength));
        return (topic, frame[(2 + topicLength)..]);
    }

    /// <summary>A MESSAGE frame: <paramref name="envelope"/> for subscription <paramref name="subscriptionId"/>.</summary>
    public static byte[] BuildMessage(ulong subscriptionId, Envelope envelope)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        byte[] frame = new byte[8 + envelope.SerializedLength];
        BinaryPrimitives.WriteUInt64LittleEndian(frame, subscriptionId);
        envelope.WriteTo(frame.AsSpan(8));
        return frame;
    }

    /// <summary>Split a MESSAGE frame into its subscription id and its envelope bytes.</summary>
    /// <exception cref="InvalidDataException">The frame is shorter than its 8-byte id.</exception>
    public static (ulong SubscriptionId, ReadOnlyMemory<byte> Envelope) ParseMessage(ReadOnlyMemory<byte> frame)
    {
        if (frame.Length < 8)
        {
            throw new InvalidDataException("message: frame too short (" + frame.Length + " bytes; the subscription id is 8)");
        }

        return (BinaryPrimitives.ReadUInt64LittleEndian(frame.Span), frame[8..]);
    }
}
