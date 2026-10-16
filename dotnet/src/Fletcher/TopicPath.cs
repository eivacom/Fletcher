// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// A topic's identity: a segment LIST, validated here so a bad one never crosses.
//
// ── Why the six rules are re-implemented in managed code ────────────────────
// The seam validates every one of them anyway, inside `RequireSegments`, and the
// shim inherits that by calling through it - so this is a SECOND copy of a
// taxonomy, which the round otherwise refuses on principle. It earns the
// exception for one reason: a refusal raised here carries a C# stack trace
// pointing at the caller's own line, and a refusal raised natively carries a
// message about a segment list the caller never typed. D-BIND-20 rules that
// trade deliberately.
//
// What is duplicated is the RULE SET, not any data: the segments cross as bytes
// exactly as they always did, and what now has two implementations is the
// predicate. So what can drift is which topics the two sides believe exist.
//
// THE MITIGATION IS THAT THERE IS NO BYPASS. `ToSegments` in the shim validates
// nothing on purpose, and `RequireSegments` runs from `JoinSegments` on every
// publisher and subscriber path, unconditionally and per call - there is no
// flag, no fast path and no "the binding already checked" parameter anywhere on
// the crossing. If these rules ever drift PERMISSIVE, the seam refuses the call
// and the cost is a message about a segment list the caller did not type.
//
// THE UNGUARDED DIRECTION IS STRICT, and it is worth naming because nothing
// downstream can catch it: a managed rule tighter than the seam's refuses a
// topic the transport would have accepted, and the call that would have proved
// it never happens. No native check can see a crossing that was not made. That
// is the residual risk of the second copy, and it belongs here rather than being
// discovered by someone whose valid topic this file rejected.
//
// ── IDENTITY IS BYTES, so validation is in UTF-8 and not in UTF-16 ──────────
// The seam's rule 6 bounds the JOINED name at 246 bytes, and "bytes" there means
// the bytes that reach the wire. A .NET string is UTF-16, so `"ä".Length == 1`
// while its encoded form is two bytes: measuring in chars would accept names the
// transport truncates, which is the exact silent failure rule 6 exists to
// prevent. Every length below is therefore an encoded byte count.
//
// The same reasoning forces STRICT encoding. A lone surrogate is a legal .NET
// string and an illegal UTF-8 sequence, and the default encoder replaces it with
// U+FFFD - so a caller's topic would silently become a DIFFERENT topic, one they
// cannot type. Refused instead, at the only place that can still name which
// segment did it.
using System;
using System.Collections.Generic;
using System.Text;

namespace Eiva.Fletcher;

/// <summary>A topic, as the segment list that is its identity.</summary>
/// <remarks>
/// Construct with <see cref="Of"/>; there is no other way, which is what gives
/// the six rules exactly one implementation on this side.
/// </remarks>
public readonly struct TopicPath : IEquatable<TopicPath>
{
    /// <summary>Fast DDS's 255-byte announced ceiling, less the 9 bytes of "/__schema".</summary>
    /// <remarks>
    /// The headroom IS the rule. Bounded at 255 the data name survives but the
    /// companion channel a provider derives from it truncates back onto it, so the
    /// collision moves to the hidden channel instead of closing - harder to find,
    /// not fixed.
    /// </remarks>
    internal const int MaxJoinedBytes = 246;

    /// <summary>Throws on a lone surrogate rather than substituting U+FFFD.</summary>
    private static readonly UTF8Encoding StrictUtf8 = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

    private readonly string[]? _segments;
    private readonly byte[][]? _utf8;
    private readonly string? _key;

    private TopicPath(string[] segments, byte[][] utf8, string key)
    {
        _segments = segments;
        _utf8 = utf8;
        _key = key;
    }

    /// <summary>Validate a segment list and make a topic of it.</summary>
    /// <exception cref="ArgumentException">Any of the seam's six rules is broken.</exception>
    public static TopicPath Of(params string[] segments)
    {
        ArgumentNullException.ThrowIfNull(segments);

        // RULE 1 - the empty LIST. There is no default topic and no recovery.
        if (segments.Length == 0)
        {
            throw new ArgumentException(
                "a topic needs at least one segment: an empty segment list names no topic",
                nameof(segments));
        }

        // The encode happens ONCE, here, and the bytes are kept. Publishing walks
        // this array rather than re-encoding per sample, so a topic used for a
        // million rows is encoded once - and the length rule below is measured on
        // exactly the bytes that will cross, not on a second encoding of them.
        var utf8 = new byte[segments.Length][];
        for (int i = 0; i < segments.Length; i++)
        {
            string? segment = segments[i];
            if (segment is null)
            {
                throw new ArgumentException($"segment {i} is null", nameof(segments));
            }

            try
            {
                utf8[i] = StrictUtf8.GetBytes(segment);
            }
            catch (EncoderFallbackException e)
            {
                // A lone surrogate. Said plainly, because the default behaviour -
                // silently substituting U+FFFD - would have made this a different
                // topic rather than an error.
                throw new ArgumentException(
                    $"segment {i} cannot be encoded as UTF-8, so it cannot name a topic: {e.Message}",
                    nameof(segments), e);
            }
        }

        // RULE 6 FIRST, matching the seam's own order, so the same input is refused
        // for the same reason on both sides. It is also the only rule whose
        // violation is otherwise SILENT rather than merely wrong.
        long joined = segments.Length - 1;  // the separators
        foreach (byte[] bytes in utf8)
        {
            joined += bytes.Length;
        }

        if (joined > MaxJoinedBytes)
        {
            throw new ArgumentException(
                $"the joined topic name is {joined} bytes, above the {MaxJoinedBytes}-byte limit " +
                "that keeps it and its companion channel from being silently truncated on the wire",
                nameof(segments));
        }

        for (int i = 0; i < segments.Length; i++)
        {
            string segment = segments[i];

            // RULE 4 - an empty segment. `{""}` reproduces the very name rule 1
            // forbids, and `{"a",""}` names "a/".
            if (segment.Length == 0)
            {
                throw new ArgumentException($"segment {i} is empty, and an empty segment names nothing", nameof(segments));
            }

            // RULE 5 - the "__" PREFIX, not the literal "__schema", so every present
            // and future provider-derived companion name is out of reach.
            if (segment.StartsWith("__", StringComparison.Ordinal))
            {
                throw new ArgumentException(
                    $"segment {i} begins \"__\", which is reserved for provider-derived companion channels: {segment}",
                    nameof(segments));
            }

            // RULE 3 - the separator. A segment containing one would name a
            // DIFFERENT segment list, which is what used to make {"a/b"} and
            // {"a","b"} one topic.
            if (segment.Contains('/'))
            {
                throw new ArgumentException(
                    $"segment {i} contains the separator '/', which would name a different segment list: {segment}",
                    nameof(segments));
            }

            // RULE 2 - a zero byte. XRCE hands the joined name to its Agent as a
            // `const char*`, which has no length form, so a NUL here is silent
            // truncation that cannot be repaired at the sink.
            if (segment.Contains('\0'))
            {
                throw new ArgumentException(
                    $"segment {i} contains a zero byte, which would truncate the name on the wire",
                    nameof(segments));
            }
        }

        var copy = new string[segments.Length];
        Array.Copy(segments, copy, segments.Length);
        return new TopicPath(copy, utf8, string.Join('/', copy));
    }

    /// <summary>The segments, in order.</summary>
    public IReadOnlyList<string> Segments => _segments ?? Array.Empty<string>();

    /// <summary>The single '/'-joined name every provider identifies this topic by.</summary>
    /// <exception cref="InvalidOperationException">This is a defaulted value.</exception>
    public string ToKey() => _key ?? throw new InvalidOperationException(
        "this TopicPath was never constructed: use TopicPath.Of(...) rather than default(TopicPath)");

    /// <summary>The per-segment UTF-8, encoded once at construction.</summary>
    internal byte[][] Utf8Segments => _utf8 ?? throw new InvalidOperationException(
        "this TopicPath was never constructed: use TopicPath.Of(...) rather than default(TopicPath)");

    /// <summary>Whether this came from <see cref="Of"/> rather than from <c>default</c>.</summary>
    /// <remarks>
    /// A struct can always be defaulted - C# offers no way to refuse it - so the
    /// hole a private constructor cannot close is closed at every USE instead.
    /// </remarks>
    internal bool IsConstructed => _segments is not null;

    /// <inheritdoc/>
    public bool Equals(TopicPath other) => string.Equals(_key, other._key, StringComparison.Ordinal);

    /// <inheritdoc/>
    public override bool Equals(object? obj) => obj is TopicPath other && Equals(other);

    /// <inheritdoc/>
    /// <remarks>
    /// Keyed on the joined name, which is exactly the seam's identity claim: the
    /// join is injective over accepted lists, so two topics are equal here if and
    /// only if every provider would treat them as one.
    /// </remarks>
    public override int GetHashCode() => _key?.GetHashCode(StringComparison.Ordinal) ?? 0;

    /// <inheritdoc/>
    public override string ToString() => _key ?? "<default>";

    /// <summary>Whether two topics name the same thing.</summary>
    public static bool operator ==(TopicPath left, TopicPath right) => left.Equals(right);

    /// <summary>Whether two topics name different things.</summary>
    public static bool operator !=(TopicPath left, TopicPath right) => !left.Equals(right);
}
