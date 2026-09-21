// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-3b: the one throw site, exercised against the real shim.
//
// The two origins are reachable without any Arrow plumbing, which is why these
// rows are cheap: fl_codec_open runs inside a CODEC-origin containment and
// fl_provider_create inside a SEAM one, so a refusal from each gives a genuine
// native error of that origin rather than a hand-built struct.
//
// WHAT IS NOT HERE YET: the full HARD-1..7 sweep. Those refusals come from the
// positional reader on malformed bytes, which needs a codec opened over a real
// schema - and the managed codec is 3c. The message-preservation property is
// asserted here on the refusals that ARE reachable; the sweep over the reader's
// taxonomy lands with the codec tier.
using System;
using System.Runtime.ExceptionServices;

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
