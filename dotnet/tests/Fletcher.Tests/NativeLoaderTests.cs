// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-3a's forcing property: the shim loads from a managed process, and the
// binding refuses a shim it was not built against.
//
// ── Why these rows exist in this order ──────────────────────────────────────
// Everything else in the round's managed half sits on top of one assumption —
// that a .NET process can find and load the native shim at all. Until that is
// asserted, every later test would be testing the codec and the loader at once,
// and a load failure would present as whatever the first codec call happened to
// be. So it is asserted here, first, on its own.
//
// ── These rows FAIL rather than SKIP when the shim is absent ────────────────
// Deliberately. A skipped test reads green, and "the native asset was never
// built" is exactly the condition that must not read green — it is the same
// mistake the shim's own single-copy scan had to avoid, where finding nothing
// looks identical to a healthy process. The failure carries the resolver's
// diagnostic, which names the RID and says to run `conan create c-abi`.
using System;
using System.Runtime.InteropServices;

using Eiva.Fletcher.Interop;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class NativeLoaderTests
{
    /// <summary>
    /// The shim loads, and its ABI version is the one this binding was compiled
    /// against.
    /// </summary>
    /// <remarks>
    /// Touching any member of <c>NativeMethods</c> runs its type initializer,
    /// which performs the handshake — so a mismatch fails this row before the
    /// assertion below is even reached, which is the behaviour the header asks
    /// for ("compare at load time").
    /// </remarks>
    [Fact]
    public void TheShimLoadsAndReportsTheAbiVersionThisBindingWasBuiltAgainst()
    {
        uint version = NativeMethods.fl_binding_abi_version();

        Assert.Equal(NativeLoader.HeaderVersion, version);
        Assert.Equal(NativeLoader.HeaderVersionMajor, version >> 16);
        Assert.Equal(NativeLoader.HeaderVersionMinor, version & 0xFFFF);
    }

    /// <summary>
    /// The single-copy marker is reachable through the ABI.
    /// </summary>
    /// <remarks>
    /// Two things at once, and the second is the reason it is here. It is a
    /// second entry point, so the surface is not a single lucky symbol; and
    /// calling it is what the shim itself relies on to keep its load-time
    /// single-copy scan from being dead-stripped by the linker. A managed caller
    /// reaching this function is the closest thing this suite has to evidence
    /// that the scan is present in the shipped artifact.
    /// </remarks>
    [Fact]
    public void TheSingleCopyMarkerIsReachableAndNotEmpty()
    {
        nint marker = NativeMethods.fl_single_copy_marker();

        Assert.NotEqual(0, marker);
        string? text = Marshal.PtrToStringUTF8(marker);
        Assert.False(string.IsNullOrEmpty(text));
        Assert.Contains("fletcher-c-abi", text, StringComparison.Ordinal);
    }

    /// <summary>
    /// The handshake accepts the version it was built against.
    /// </summary>
    [Fact]
    public void TheHandshakeAcceptsItsOwnVersion() =>
        NativeLoader.VerifyAbiVersion(NativeLoader.HeaderVersion);

    /// <summary>
    /// The handshake REFUSES anything else, and this is the row that makes the
    /// one above mean something.
    /// </summary>
    /// <remarks>
    /// A version check nobody has watched reject a version is not a check. The
    /// cases below are the three shapes that matter, and the third is the one a
    /// semantic-versioning habit would wave through: while MAJOR is 0, `binding.h`
    /// gives NO compatibility guarantee at all, so a NEWER minor is refused too.
    /// If this binding ever reaches 1.0, that case becomes legal and this row has
    /// to be revisited on purpose rather than discovered.
    /// </remarks>
    /// <remarks>
    /// DERIVED from the header's own constants rather than written out, and that
    /// is not tidiness: the cases were literals until D-BIND-42 bumped the minor
    /// from 1 to 2, at which point `(0, 2)` — listed here as "a newer minor,
    /// refused" — became the version the shim actually reports, and this row
    /// failed. It failing was the right outcome; it needing a hand-edit on every
    /// ABI bump was not. Each case is now an offset from the real version, so the
    /// next bump moves them with it.
    /// </remarks>
    [Theory]
    [InlineData(NativeLoader.HeaderVersionMajor + 1, NativeLoader.HeaderVersionMinor)]
    [InlineData(NativeLoader.HeaderVersionMajor, NativeLoader.HeaderVersionMinor - 1)]
    [InlineData(NativeLoader.HeaderVersionMajor, NativeLoader.HeaderVersionMinor + 1)]
    public void TheHandshakeRefusesAnyOtherVersion(uint major, uint minor)
    {
        // None of the three is the matching version, by construction: each is an
        // offset from it, and the matching one is asserted by the row above.
        uint packed = (major << 16) | minor;

        InvalidOperationException error =
            Assert.Throws<InvalidOperationException>(() => NativeLoader.VerifyAbiVersion(packed));

        // The message has to name both versions, or its reader cannot tell which
        // half of the pair is the odd one.
        Assert.Contains($"{major}.{minor}", error.Message, StringComparison.Ordinal);
        Assert.Contains(
            $"{NativeLoader.HeaderVersionMajor}.{NativeLoader.HeaderVersionMinor}",
            error.Message,
            StringComparison.Ordinal);
    }

    /// <summary>
    /// The resolver answers for Fletcher's shim and for nothing else.
    /// </summary>
    /// <remarks>
    /// It is installed per-assembly, but it is still a hook in a process that
    /// loads other native libraries. Returning anything but zero for a name that
    /// is not ours would put this package in the way of somebody else's
    /// dependency.
    /// </remarks>
    [Fact]
    public void TheResolverDoesNotAnswerForOtherLibraries()
    {
        nint handle = NativeLoader.Resolve(
            "some-other-native-library",
            typeof(NativeLoader).Assembly,
            searchPath: null);

        Assert.Equal(0, handle);
    }
}
