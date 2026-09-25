// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-3a's handle lifetimes, exercised against the real shim.
//
// These rows are about ORDER and OWNERSHIP rather than about anything the codec
// computes. The values crossing are incidental; what is under test is that a
// handle releases exactly once, that releasing twice is safe, that a failed
// creation hands back nothing to release, and that a publisher keeps the provider
// it borrows alive even when the caller disposes them in the wrong order.
using System;

using Eiva.Fletcher.Interop;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class HandleTests
{
    /// <summary>
    /// A publisher keeps its provider alive, even when the caller disposes the
    /// provider first.
    /// </summary>
    /// <remarks>
    /// The forcing row for the pinning in <c>PublisherHandle.Create</c>. The
    /// header says a publisher BORROWS its provider and the provider must outlive
    /// it; this disposes them in exactly the wrong order and requires the process
    /// to survive.
    ///
    /// Without the pin, <c>provider.Dispose()</c> would run
    /// <c>fl_provider_destroy</c> immediately and the <c>fl_publisher_destroy</c>
    /// below would touch freed memory — a failure that ends the test process
    /// rather than failing a row, and so cannot be asserted directly. It does not
    /// need to be: the pin is observable on its own terms. After disposing the
    /// provider it is NOT closed, because the publisher holds a reference, and it
    /// closes only once the publisher is released. Those two assertions are the
    /// mechanism rather than a proxy for it.
    ///
    /// The native side is belt-and-braces about the same rule: `fl_publisher`
    /// holds a `shared_ptr` share of the provider, so the ordering is survivable
    /// on both sides of the boundary rather than only on ours.
    /// </remarks>
    [Fact]
    public void APublisherKeepsItsProviderAliveEvenIfTheProviderIsDisposedFirst()
    {
        ProviderHandle provider = CreateInProcessProvider();
        FlError err = default;

        int status = PublisherHandle.Create(provider, out PublisherHandle publisher, ref err);
        Assert.Equal(0, status);
        Assert.False(publisher.IsInvalid);

        // The wrong order, deliberately.
        provider.Dispose();

        // AND THE PIN IS DIRECTLY OBSERVABLE HERE: the provider is NOT closed,
        // because the publisher still holds a reference on it. This assertion is
        // the mechanism itself rather than a proxy for it - contrast
        // DisposingAHandleTwiceIsSafe below, where a provider with no publisher
        // closes on the first Dispose.
        Assert.False(provider.IsClosed);

        publisher.Dispose();
        Assert.True(publisher.IsClosed);

        // Releasing the borrower releases the borrow, and only then does the
        // provider close - so fl_provider_destroy runs AFTER fl_publisher_destroy
        // whatever order the caller asked for.
        Assert.True(provider.IsClosed);
    }

    /// <summary>Disposing a handle twice is safe, and the second is a no-op.</summary>
    /// <remarks>
    /// Worth a row of its own because <c>ReleaseHandle</c> calls a native destroy
    /// that would be a double free if <see cref="System.Runtime.InteropServices.SafeHandle"/>
    /// did not guarantee it runs once. A managed tier built on these will dispose
    /// from a <c>using</c> and again from a finalizer often enough.
    /// </remarks>
    [Fact]
    public void DisposingAHandleTwiceIsSafe()
    {
        ProviderHandle provider = CreateInProcessProvider();

        provider.Dispose();
        provider.Dispose();

        Assert.True(provider.IsClosed);
    }

    /// <summary>
    /// A refused creation hands back an INVALID handle, not a live one.
    /// </summary>
    /// <remarks>
    /// The other half of the ownership contract: on failure there is nothing to
    /// release, and the handle must say so rather than carrying a stale or zero
    /// pointer that something later tries to close. A NULL schema is the cheapest
    /// refusal the codec has.
    /// </remarks>
    [Fact]
    public void ARefusedCodecOpenYieldsAnInvalidHandleAndAMessage()
    {
        FlError err = default;

        int status = NativeMethods.fl_codec_open(nint.Zero, out CodecHandle codec, ref err);

        Assert.NotEqual(0, status);
        Assert.True(codec.IsInvalid);

        // The message crosses too — the number alone is not enough, which is the
        // whole reason fl_error carries bytes (D-BIND-19 rule 1).
        Assert.NotEqual(0, err.Message);
        Assert.NotEqual(0u, (uint)err.MessageLen);

        NativeMethods.fl_error_dispose(ref err);

        // Disposed means zeroed: the struct is safe to reuse and safe to dispose
        // again, which is what lets the managed throw site be an unconditional
        // finally.
        Assert.Equal(0, err.Message);
        Assert.Equal(0, err.Status);
        NativeMethods.fl_error_dispose(ref err);

        codec.Dispose();
    }

    /// <summary>An invalid handle releases nothing when disposed.</summary>
    [Fact]
    public void AnInvalidHandleIsSafeToDispose()
    {
        FlError err = default;
        int status = NativeMethods.fl_codec_open(nint.Zero, out CodecHandle codec, ref err);
        Assert.NotEqual(0, status);
        NativeMethods.fl_error_dispose(ref err);

        // SafeHandle skips ReleaseHandle for an invalid handle, so this must not
        // reach fl_codec_close with a null pointer.
        codec.Dispose();
        Assert.True(codec.IsClosed);
    }

    private static unsafe ProviderHandle CreateInProcessProvider()
    {
        ReadOnlySpan<byte> selector = "inprocess"u8;
        fixed (byte* bytes = selector)
        {
            FlStr name = new() { Data = (nint)bytes, Len = (nuint)selector.Length };
            FlProviderConfig config = default;
            FlError err = default;

            int status = NativeMethods.fl_provider_create(name, in config, out ProviderHandle provider, ref err);

            Assert.Equal(0, status);
            Assert.False(provider.IsInvalid);
            return provider;
        }
    }
}
