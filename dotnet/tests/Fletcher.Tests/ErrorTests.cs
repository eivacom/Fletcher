// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-3b and BIND-3c: the one throw site, exercised against the real shim.
//
// The two origins are reachable without any Arrow plumbing, which is why the
// first rows here are cheap: fl_codec_open runs inside a CODEC-origin
// containment and fl_provider_create inside a SEAM one, so a refusal from each
// gives a genuine native error of that origin rather than a hand-built struct.
//
// ADDED AT 3c, as 3b said it would be: the HARD-1..7 sweep. Those refusals come
// from the positional reader on malformed bytes, which needs a codec opened over
// a real schema, and the managed codec did not exist until this slice. The
// property they carry is the one D-BIND-1c locks - the diagnostics HARD-1..7 put
// into the reader survive the crossing WORD FOR WORD, because a message that
// says which byte and why is the difference between a five-second fix and an
// afternoon, and a binding that replaced it with a taxonomy of its own would
// throw that away while still passing every status assertion.
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.ExceptionServices;

using Apache.Arrow;

using Eiva.Fletcher.Interop;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class ErrorTests
{
    /// <summary>A codec-origin failure is a FletcherFormatException.</summary>
    /// <remarks>
    /// The typed subclass is the whole reason `fl_error` carries an origin at all:
    /// the NUMBER cannot separate an InvalidArgument from the seam ("no such
    /// provider") from one from the reader on a truncated buffer, and a caller
    /// acts on those differently (D-BIND-15).
    /// </remarks>
    [Fact]
    public void ACodecOriginFailureIsAFormatException()
    {
        FlError err = default;
        int status = NativeMethods.fl_codec_open(nint.Zero, out CodecHandle codec, ref err);
        codec.Dispose();

        FletcherFormatException error =
            Assert.Throws<FletcherFormatException>(() => Errors.ThrowIfFailed(status, ref err));

        Assert.Equal(FletcherOrigin.Codec, error.Origin);
        Assert.Equal(FletcherStatus.InvalidArgument, error.Status);
        Assert.Contains("fl_codec_open", error.Message, StringComparison.Ordinal);
    }

    /// <summary>A seam-origin failure is a plain FletcherException, not the subclass.</summary>
    /// <remarks>
    /// The other half of the mapping, and the row that makes the one above mean
    /// something: if every failure became a FletcherFormatException the typed
    /// subclass would carry no information.
    /// </remarks>
    [Fact]
    public void ASeamOriginFailureIsNotAFormatException()
    {
        FlError err = default;
        int status = CreateProvider("no-such-provider", ref err, out ProviderHandle provider);
        provider.Dispose();

        FletcherException error =
            Assert.Throws<FletcherException>(() => Errors.ThrowIfFailed(status, ref err));

        Assert.IsType<FletcherException>(error, exactMatch: true);
        Assert.Equal(FletcherOrigin.Seam, error.Origin);
    }

    /// <summary>
    /// The native message crosses VERBATIM, and it is worth carrying.
    /// </summary>
    /// <remarks>
    /// D-BIND-19 rule 1: the number alone is not enough. A registry refusal names
    /// every provider that IS registered, which is the difference between a
    /// five-second fix and an afternoon — so this asserts the message contains
    /// what the shim put there, not merely that it is non-empty.
    /// </remarks>
    [Fact]
    public void TheNativeMessageIsCarriedVerbatim()
    {
        FlError err = default;
        int status = CreateProvider("no-such-provider", ref err, out ProviderHandle provider);
        provider.Dispose();

        FletcherException error =
            Assert.Throws<FletcherException>(() => Errors.ThrowIfFailed(status, ref err));

        Assert.Contains("no-such-provider", error.Message, StringComparison.Ordinal);

        // The registry's refusal lists what IS available. If that were dropped the
        // message would still be non-empty and would still pass a weaker test.
        Assert.Contains("inprocess", error.Message, StringComparison.Ordinal);
    }

    /// <summary>
    /// A captured managed exception is rethrown AS ITSELF, outranking the status.
    /// </summary>
    /// <remarks>
    /// D-BIND-19 rule 3. A user's own exception thrown from their own writer comes
    /// back with its own type and its own stack rather than wrapped in a
    /// FletcherException, because having crossed a boundary should not rewrite the
    /// managed caller's contract. Native still saw a real failure and logged it.
    /// </remarks>
    [Fact]
    public void ACapturedManagedExceptionIsRethrownAsItself()
    {
        FlError err = default;
        int status = NativeMethods.fl_codec_open(nint.Zero, out CodecHandle codec, ref err);
        codec.Dispose();

        InvalidTimeZoneException original = new("the user's own failure");
        ExceptionDispatchInfo captured = ExceptionDispatchInfo.Capture(original);

        // The status says InvalidArgument from the CODEC, which would otherwise
        // produce a FletcherFormatException. The captured exception outranks it.
        InvalidTimeZoneException thrown = Assert.Throws<InvalidTimeZoneException>(
            () => Errors.ThrowIfFailed(status, ref err, captured));

        Assert.Same(original, thrown);
    }

    /// <summary>Success throws nothing and leaves the error alone.</summary>
    [Fact]
    public void SuccessThrowsNothing()
    {
        FlError err = default;
        Errors.ThrowIfFailed(0, ref err);
    }

    /// <summary>
    /// The message is released on every failing path, including the rethrow one.
    /// </summary>
    /// <remarks>
    /// The dispose is a <c>finally</c> precisely so that rule 3's early throw does
    /// not skip it. Observable because <c>fl_error_dispose</c> zeroes the struct:
    /// after the throw, the message pointer is gone.
    /// </remarks>
    [Fact]
    public void TheMessageIsReleasedEvenWhenACapturedExceptionWins()
    {
        FlError err = default;
        int status = NativeMethods.fl_codec_open(nint.Zero, out CodecHandle codec, ref err);
        codec.Dispose();
        Assert.NotEqual(0, err.Message);

        ExceptionDispatchInfo captured = ExceptionDispatchInfo.Capture(new InvalidTimeZoneException());
        Assert.Throws<InvalidTimeZoneException>(() => Errors.ThrowIfFailed(status, ref err, captured));

        Assert.Equal(0, err.Message);
        Assert.Equal(0, err.Status);
    }

    // ── the HARD-1..7 sweep (BIND-3c) ───────────────────────────────────────

    /// <summary>The reader's own message reaches the managed caller, verbatim.</summary>
    /// <remarks>
    /// The sweep walks a four-byte 0xFF window across a valid encoding and
    /// requires every refusal it provokes to carry the POSITIONAL READER's
    /// message. That is a stronger property than "malformed input throws", and it
    /// is the one worth holding: if the binding ever refused bytes on its own
    /// account — a length check of its own, a friendlier wrapper message — then
    /// the HARD-1..7 hardening would stop covering the binding, and the day a
    /// truncated buffer arrived in production the message would say nothing about
    /// which byte.
    ///
    /// Mutations the format happens to ACCEPT are not failures of this test: the
    /// bytes are still a well-formed encoding of different values. The guard at
    /// the end is what stops that from becoming a sweep that refuses nothing and
    /// asserts nothing — the tautology this round has already shipped once and
    /// caught (the 142/142 that scored a producer against the span it had lent).
    /// </remarks>
    [Fact]
    public void EveryHardCaseKeepsItsMessage()
    {
        RecordBatch batch = CodecFixtures.Composite();
        using var codec = new FletcherCodec(batch.Schema);

        byte[] valid;
        using (BoundRows rows = codec.Bind(batch))
        {
            valid = CodecFixtures.EncodeAll(codec, rows);
        }

        Assert.True(valid.Length > 4, "the fixture encoded to too little to mutate meaningfully");

        int refused = 0;
        var statuses = new HashSet<FletcherStatus>();
        for (int offset = 0; offset + 4 <= valid.Length; ++offset)
        {
            byte[] corrupt = (byte[])valid.Clone();
            for (int b = 0; b < 4; ++b)
            {
                corrupt[offset + b] = 0xFF;
            }

            try
            {
                using RecordBatch decoded = codec.DecodeBatch(corrupt, batch.Length);
            }
            catch (FletcherFormatException refusal)
            {
                ++refused;
                statuses.Add(refusal.Status);

                Assert.StartsWith("PositionalReader:", refusal.Message, StringComparison.Ordinal);
                Assert.Equal(FletcherOrigin.Codec, refusal.Origin);
            }
        }

        Assert.True(refused > 0, "no mutation was refused, so this row asserted nothing");

        // Malformed BYTES are the reader's to refuse, and the reader has exactly
        // one number for that. A second status appearing here would mean some
        // other frame decided something the reader should have.
        Assert.Equal([FletcherStatus.InvalidArgument], statuses);
    }

    /// <summary>
    /// A malformed refusal is the FORMAT exception, not the general one, and a
    /// caller can act on the difference.
    /// </summary>
    /// <remarks>
    /// The typed subclass is the whole reason <c>fl_error</c> carries an origin.
    /// A transport failure is worth retrying; malformed bytes never are, and the
    /// catch clause that separates them is the only place that distinction can be
    /// made without parsing a message.
    /// </remarks>
    [Fact]
    public void ATruncatedBufferIsRefusedAtEveryTruncationPoint()
    {
        RecordBatch batch = CodecFixtures.Composite();
        using var codec = new FletcherCodec(batch.Schema);

        var output = new ArrayBufferWriter<byte>();
        using (BoundRows rows = codec.Bind(batch))
        {
            codec.Encode(rows, 0, output);
        }

        byte[] valid = output.WrittenSpan.ToArray();

        // The null bitfield sits at the front and is untouched by a prefix cut, so
        // every field the whole row read a shorter one must also read — and must
        // run out. Deterministic, not probabilistic.
        for (int length = 0; length < valid.Length; ++length)
        {
            byte[] truncated = valid[..length];
            FletcherFormatException refusal = Assert.Throws<FletcherFormatException>(
                () => codec.Decode(truncated));
            Assert.StartsWith("PositionalReader:", refusal.Message, StringComparison.Ordinal);
        }

        // The control: the untruncated buffer decodes. Without it, a decoder that
        // refused everything would pass the loop above.
        using RecordBatch decoded = codec.Decode(valid);
        Assert.Equal(1, decoded.Length);
    }

    /// <summary>A count and a buffer that disagree is a refusal, never short data.</summary>
    /// <remarks>
    /// This is how a framing bug reaches production silently: the decoder reads
    /// the rows it was told about, the trailing bytes are nobody's, and the
    /// application sees a batch that is merely smaller than it should be.
    /// </remarks>
    [Fact]
    public void ABufferWithRowsLeftOverIsRefused()
    {
        RecordBatch batch = CodecFixtures.Composite();
        using var codec = new FletcherCodec(batch.Schema);

        byte[] all;
        using (BoundRows rows = codec.Bind(batch))
        {
            all = CodecFixtures.EncodeAll(codec, rows);
        }

        Assert.Throws<FletcherFormatException>(() => codec.DecodeBatch(all, batch.Length - 1));

        // And the mirror: more rows claimed than the buffer holds.
        Assert.Throws<FletcherFormatException>(() => codec.DecodeBatch(all, batch.Length + 1));
    }

    private static unsafe int CreateProvider(string selector, ref FlError err, out ProviderHandle provider)
    {
        byte[] bytes = System.Text.Encoding.UTF8.GetBytes(selector);
        fixed (byte* p = bytes)
        {
            FlStr name = new() { Data = (nint)p, Len = (nuint)bytes.Length };
            FlProviderConfig config = default;
            return NativeMethods.fl_provider_create(name, in config, out provider, ref err);
        }
    }
}
