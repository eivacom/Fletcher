// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The P/Invoke surface over `c-abi/include/fletcher/abi/binding.h`.
//
// ── What this file is, and what it deliberately is not ──────────────────────
// A MECHANICAL MIRROR of the header, and nothing else. No policy, no taxonomy,
// no convenience: a declaration here has the same name, the same parameters and
// the same order as the C one, so the two can be diffed by eye. Everything that
// interprets a status number, decides what to throw, or owns a handle lives one
// layer up in `Eiva.Fletcher`, where it can be read without a C header open
// beside it.
//
// `LibraryImport` rather than `DllImport`: it generates the marshalling at
// compile time instead of at runtime, which is what makes the surface
// trim-and-AOT safe (N-8, BIND-6's concern) and what turns a non-blittable
// parameter into a BUILD error rather than a silent runtime stub. That is worth
// more here than anywhere else in the round - every type below crosses a C
// boundary, and the compiler checking that claim is cheaper than a test that
// only fails on one platform.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Eiva.Fletcher.Interop;

/// <summary>Raw entry points of the Fletcher binding ABI.</summary>
/// <remarks>
/// Internal on purpose: the shim's surface is not a supported API. Applications
/// use <c>Eiva.Fletcher</c>, which is where handles get lifetimes and status
/// numbers get exceptions.
/// </remarks>
internal static partial class NativeMethods
{
    /// <summary>
    /// The ABI version handshake, run once before any other entry point is
    /// reachable.
    /// </summary>
    /// <remarks>
    /// `binding.h` asks for exactly this and names this assembly while doing it:
    /// *"Compare against the loaded shim's fl_binding_abi_version() at load time;
    /// Eiva.Fletcher.Interop does exactly that in its static constructor."*
    ///
    /// A type initializer is the right hook because the runtime guarantees it has
    /// completed before any member of this class is used — including the very
    /// first P/Invoke — so no entry point below can be reached against an
    /// unverified shim. Calling one of this class's own imports from inside its
    /// initializer is legal and well-defined: the initializer is already marked
    /// running on this thread, so the call proceeds rather than recursing.
    /// </remarks>
    static NativeMethods()
    {
        // Order matters and is the whole reason these two lines share a method:
        // the resolver has to be in place before the first import resolves, and
        // the first import to resolve is the version probe on the next line.
        NativeLoader.Install();
        NativeLoader.VerifyAbiVersion(fl_binding_abi_version());
    }

    /// <summary>
    /// The name every <see cref="LibraryImportAttribute"/> below resolves, and the
    /// name <see cref="NativeLoader"/> answers for.
    /// </summary>
    /// <remarks>
    /// No extension and no `lib` prefix: the runtime applies each platform's own
    /// convention, so this one string covers <c>fletcher-c-abi.dll</c> and
    /// <c>libfletcher-c-abi.so</c>.
    /// </remarks>
    internal const string LibraryName = "fletcher-c-abi";

    /// <summary>
    /// The loaded shim's ABI version, packed as <c>(major &lt;&lt; 16) | minor</c>.
    /// </summary>
    /// <remarks>
    /// The first call into the shim in any process, and the one the handshake is
    /// built on. It takes no arguments, returns a scalar, touches no state and
    /// cannot fail, which is exactly what a version probe has to be: anything
    /// richer could not be called safely against a shim whose version is not yet
    /// known to be compatible.
    /// </remarks>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial uint fl_binding_abi_version();

    /// <summary>The single-copy marker text (D-BIND-17).</summary>
    /// <remarks>
    /// Returns a NUL-terminated string owned by the shim and valid for the life of
    /// the module, so the marshaller may point at it without copying. Declared
    /// here because touching it is what keeps the shim's load-time single-copy
    /// scan from being dead-stripped; the managed side never needs its value.
    /// </remarks>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    internal static partial nint fl_single_copy_marker();
}
