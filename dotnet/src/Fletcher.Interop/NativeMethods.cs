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
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial uint fl_binding_abi_version();

    /// <summary>The single-copy marker text (D-BIND-17).</summary>
    /// <remarks>
    /// Returns a NUL-terminated string owned by the shim and valid for the life of
    /// the module, so the marshaller may point at it without copying. Declared
    /// here because touching it is what keeps the shim's load-time single-copy
    /// scan from being dead-stripped; the managed side never needs its value.
    /// </remarks>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial nint fl_single_copy_marker();

    /* Errors ---------------------------------------------------------------- */

    /// <summary>Release the message an <see cref="FlError"/> carries and zero it.</summary>
    /// <remarks>
    /// Safe on a zeroed struct and safe to call twice, which is what lets the
    /// managed throw site be an unconditional <c>finally</c>. For errors the SHIM
    /// filled only, never for one a managed callback filled (D-BIND-32).
    /// </remarks>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void fl_error_dispose(ref FlError err);

    /* Owned string lists ---------------------------------------------------- */

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial nuint fl_string_list_size(nint list);

    /// <summary>The item at <paramref name="index"/>, BORROWED from the list.</summary>
    /// <remarks>
    /// Out of range yields a NULL/0 sentinel rather than a status: the signature
    /// has nowhere to put one, and the caller learns the size from
    /// <c>fl_string_list_size</c>. BIND-1 review DEBT D2 notes the header leaves
    /// this unspecified where its twin specifies it; the shim does return the
    /// sentinel.
    /// </remarks>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial FlStr fl_string_list_at(nint list, nuint index);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void fl_string_list_dispose(nint list);

    /* The codec: open once, bind once per batch, then encode or decode ------- */

    /// <summary>Open a codec over a schema. The schema is BORROWED and deep-copied.</summary>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int fl_codec_open(nint schema, out nint codec, ref FlError err);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void fl_codec_close(nint codec);

    /// <summary>Bind one Arrow array to a codec, validated once for the batch.</summary>
    /// <remarks>
    /// The array is BORROWED AND NEVER CONSUMED: the codec does not call its
    /// release callback, and the caller keeps the export alive until
    /// <c>fl_rows_unbind</c> and releases it afterwards. That rule is what lets ONE
    /// export serve N publishes, and the managed tier is expected to make it
    /// structural - BoundRows.Dispose unbinds and THEN releases.
    /// </remarks>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int fl_rows_bind(nint codec, nint array, out nint rows, ref FlError err);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void fl_rows_unbind(nint rows);

    /// <summary>Encode row <paramref name="i"/> into a caller-supplied window.</summary>
    /// <remarks>
    /// The route for bytes in hand. The zero-copy route is
    /// <c>fl_publisher_publish_row</c>, and no encode entry point returns bytes.
    /// </remarks>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int fl_encode_row(nint rows, long i, ref FlWriteWindow sink, ref FlError err);

    /// <summary>Decode <paramref name="count"/> rows into a fresh array the caller OWNS.</summary>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int fl_decode_rows(
        nint codec, nint bytes, nuint len, long count, nint output, ref FlError err);

    /* Provider and publisher ------------------------------------------------ */

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int fl_provider_create(
        FlStr selector, in FlProviderConfig config, out nint provider, ref FlError err);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void fl_provider_destroy(nint provider);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int fl_publisher_create(nint provider, out nint publisher, ref FlError err);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial void fl_publisher_destroy(nint publisher);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int fl_publisher_create_topic(
        nint publisher, FlTopic topic, nint schema, ref FlError err);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int fl_publisher_list_topics(nint publisher, out nint list, ref FlError err);

    /// <summary>Publish bytes a producer writes straight into the transport window.</summary>
    /// <remarks>
    /// <paramref name="writer"/> is an UnmanagedCallersOnly function pointer, not a
    /// delegate: the writer runs inside the seam publish, and a delegate would put
    /// a marshalling stub and a GC handle on the one path that exists to avoid a
    /// copy. A writer reporting 0 bytes is how a binding signals that its own thunk
    /// captured an exception (D-BIND-19 rule 3).
    /// </remarks>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int fl_publisher_publish_raw(
        nint publisher, FlTopic topic, nint writer, nint ctx, nuint minBytes, nint atts, ref FlError err);

    /// <summary>Publish row <paramref name="i"/>, the fused zero-copy path.</summary>
    /// <remarks>
    /// The codec runs INSIDE the seam publish, writing into the provider window, so
    /// no intermediate bytes exist. <paramref name="atts"/> may be NULL, and in this
    /// slice it can be nothing else: nothing constructs an attachments set until
    /// BIND-4 (D-BIND-31).
    /// </remarks>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int fl_publisher_publish_row(
        nint publisher, FlTopic topic, nint rows, long i, nint atts, ref FlError err);

    /// <summary>Publish rows [first, first + count), N samples in one crossing.</summary>
    /// <remarks>
    /// Partial publication is NOT unwound and cannot be: rows already handed to the
    /// transport have gone out. A failure at row k means the rows before it were
    /// published, which the caller must treat as a resend decision rather than a
    /// rollback.
    /// </remarks>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    internal static partial int fl_publisher_publish_rows(
        nint publisher, FlTopic topic, nint rows, long first, long count, nint attsPerRow, ref FlError err);
}
