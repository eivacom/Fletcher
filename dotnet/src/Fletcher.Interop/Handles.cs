// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// One SafeHandle per opaque ABI handle.
//
// ── Why SafeHandle rather than a raw nint with a Dispose ────────────────────
// Not tidiness. A raw pointer held in a managed object is leaked by any path
// that skips Dispose - an exception between construction and the try, a process
// tearing down, a caller who forgot - and the shim has no way to notice. A
// SafeHandle is finalizable and is understood by the runtime: it is kept alive
// across a P/Invoke that receives it, so the collector cannot free the wrapper
// while native code is still using the pointer, and that hazard is real here
// because most of these handles are passed and then not touched again for the
// duration of a long call.
//
// ── The part SafeHandle CANNOT do, stated rather than glossed ───────────────
// `ReleaseHandle` runs on the FINALIZER thread at a moment nobody chooses. For
// the codec, the bound rows and a string list that is fine: the header attaches
// no ordering or threading obligation to closing them. For the PROVIDER it is
// not, and the header says so plainly:
//
//     "Quiescence is the caller's obligation: no delivery may be in flight and
//      no publisher or subscriber may still hold it."
//
// A finalizer honours neither clause by itself. This file closes the second one
// with SafeHandle's own reference counting - a publisher AddRefs the provider it
// was created from, so the provider's ReleaseHandle cannot run while a publisher
// is alive, whatever order the two become garbage in. Holding a managed
// REFERENCE would not be enough: when both are unreachable in the same
// collection, both are finalized and the order is unspecified.
//
// The FIRST clause - no delivery in flight - cannot be solved here at all,
// because nothing in this slice can deliver. It belongs to BIND-4's thunk
// discipline (D-BIND-18: an in-flight counter that frees the GCHandle), and it
// is recorded here so that whoever builds the subscriber knows this file left
// that half open on purpose rather than by oversight.
using System;
using System.Runtime.InteropServices;

namespace Eiva.Fletcher.Interop;

/// <summary>Base for every opaque handle the binding ABI hands out.</summary>
/// <remarks>
/// Only NULL is invalid. The ABI has no -1 sentinel, so
/// <c>SafeHandleZeroOrMinusOneIsInvalid</c> would describe a contract the header
/// does not have.
/// </remarks>
internal abstract class FletcherHandle : SafeHandle
{
    private protected FletcherHandle()
        : base(IntPtr.Zero, ownsHandle: true)
    {
    }

    /// <inheritdoc/>
    public override bool IsInvalid => handle == IntPtr.Zero;
}

/// <summary>An <c>fl_codec*</c>: a schema-bound field plan, opened once per schema.</summary>
/// <remarks>
/// Immutable after construction, so any number of threads may use one
/// concurrently without a lock.
/// </remarks>
internal sealed class CodecHandle : FletcherHandle
{
    /// <inheritdoc/>
    protected override bool ReleaseHandle()
    {
        NativeMethods.fl_codec_close(handle);
        return true;
    }
}

/// <summary>An <c>fl_rows*</c>: one Arrow array bound to a codec.</summary>
/// <remarks>
/// Releasing this unbinds the VIEW only. The caller's exported
/// <c>ArrowArray</c> is BORROWED and stays the caller's to release, which is what
/// lets one export serve N publishes — and the header asks the managed tier to
/// make the order structural by unbinding and THEN releasing the export. <see
/// cref="Bind"/> does that the way <see cref="PublisherHandle.Create"/> pins a
/// provider: the export is reference-counted by this handle, so its release
/// cannot run before this unbind whatever order the two are finalized in.
///
/// No reference is taken on the codec, deliberately, and the reason is on the
/// native side: <c>fl_rows</c> holds a <c>shared_ptr</c> share of the codec, so
/// closing the codec before unbinding the rows is survivable by construction. A
/// managed AddRef would add a second, weaker guard over a hazard the shim already
/// removed — and would impose an ordering the header does not require.
/// </remarks>
internal sealed partial class BoundRowsHandle : FletcherHandle
{
    private SafeHandle? _export;
    private bool _addedRef;

    /// <summary>Bind one Arrow array to a codec, pinning the export to the result.</summary>
    /// <remarks>
    /// THE ONLY WAY TO GET A BOUND-ROWS HANDLE, for the same reason
    /// <c>fl_publisher_create</c> is private to <see cref="PublisherHandle"/>: a
    /// rule that has to be remembered at each call site is one that will be
    /// forgotten at the next one. The import below is private to this class, so
    /// no other type in the assembly can bind without the export pinned.
    ///
    /// The array is passed as a <see cref="SafeHandle"/> rather than an
    /// <c>nint</c>: the marshaller then keeps it alive for the duration of the
    /// bind, which is exactly the window in which the validation reads every
    /// buffer it points at.
    /// </remarks>
    [LibraryImport(NativeMethods.LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static partial int fl_rows_bind(
        CodecHandle codec, SafeHandle array, out BoundRowsHandle rows, ref FlError err);

    internal static int Bind(
        CodecHandle codec, SafeHandle array, out BoundRowsHandle rows, ref FlError err)
    {
        int status = fl_rows_bind(codec, array, out rows, ref err);
        if (status == 0 && !rows.IsInvalid)
        {
            rows.Pin(array);
        }

        return status;
    }

    /// <summary>Pin the export this view borrows.</summary>
    private void Pin(SafeHandle export)
    {
        bool taken = false;
        export.DangerousAddRef(ref taken);
        _export = export;
        _addedRef = taken;
    }

    /// <inheritdoc/>
    protected override bool ReleaseHandle()
    {
        NativeMethods.fl_rows_unbind(handle);

        // Unbind FIRST, release the export afterwards — the order `binding.h`
        // asks a binding to express in a type rather than in a comment.
        if (_addedRef && _export is not null)
        {
            _export.DangerousRelease();
            _addedRef = false;
        }

        return true;
    }
}

/// <summary>An <c>fl_provider*</c>.</summary>
/// <remarks>
/// See this file's header for what finalization can and cannot honour of the
/// quiescence obligation.
/// </remarks>
internal sealed class ProviderHandle : FletcherHandle
{
    /// <inheritdoc/>
    protected override bool ReleaseHandle()
    {
        NativeMethods.fl_provider_destroy(handle);
        return true;
    }
}

/// <summary>An <c>fl_publisher*</c>, which keeps its provider alive.</summary>
/// <remarks>
/// The header says a publisher BORROWS its provider and the provider must
/// outlive it. <see cref="Create"/> makes that true through SafeHandle's
/// reference count rather than through a managed reference, because reachability
/// does not order finalization: if the publisher and the provider become garbage
/// in the same collection, both are finalized and the order is unspecified. An
/// AddRef is honoured by <see cref="SafeHandle"/> itself — the provider's
/// <c>ReleaseHandle</c> will not run until the matching release.
/// </remarks>
internal sealed partial class PublisherHandle : FletcherHandle
{
    private ProviderHandle? _provider;
    private bool _addedRef;

    /// <summary>
    /// Create a publisher over a provider, and pin the provider to it.
    /// </summary>
    /// <remarks>
    /// THE ONLY WAY TO GET A PUBLISHER HANDLE, and that is the point. The pinning
    /// below has to happen at every creation site or the borrow rule is unguarded,
    /// and a rule that must be REMEMBERED at each call site is one that will be
    /// forgotten at the next one. Folding it into the creation makes it
    /// structural: <see cref="Pin"/> is private, so there is no way to obtain one
    /// of these handles without the provider already pinned to it.
    ///
    /// The same argument the header makes for BoundRows.Dispose unbinding before
    /// it releases: express the ordering in the type, not in a comment.
    /// </remarks>
    /// <remarks>
    /// The import is PRIVATE TO THIS CLASS rather than sitting with its siblings
    /// in <c>NativeMethods</c>, which is what turns "remember to pin" into
    /// "cannot be called without pinning": no other type in the assembly can
    /// reach it.
    /// </remarks>
    [LibraryImport(NativeMethods.LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static partial int fl_publisher_create(
        ProviderHandle provider, out PublisherHandle publisher, ref FlError err);

    internal static int Create(ProviderHandle provider, out PublisherHandle publisher, ref FlError err)
    {
        int status = fl_publisher_create(provider, out publisher, ref err);
        if (status == 0 && !publisher.IsInvalid)
        {
            publisher.Pin(provider);
        }

        return status;
    }

    /// <summary>Pin the provider this publisher borrows.</summary>
    private void Pin(ProviderHandle owner)
    {
        // DangerousAddRef is "dangerous" only in that it must be paired. It is
        // paired below, in the one place this handle can be released.
        bool taken = false;
        owner.DangerousAddRef(ref taken);
        _provider = owner;
        _addedRef = taken;
    }

    /// <inheritdoc/>
    protected override bool ReleaseHandle()
    {
        NativeMethods.fl_publisher_destroy(handle);

        // After the publisher is gone, and only then: the borrow ends where the
        // borrower does.
        if (_addedRef && _provider is not null)
        {
            _provider.DangerousRelease();
            _addedRef = false;
        }

        return true;
    }
}

/// <summary>An <c>fl_string_list*</c>: an owned, immutable list of byte strings.</summary>
internal sealed class StringListHandle : FletcherHandle
{
    /// <inheritdoc/>
    protected override bool ReleaseHandle()
    {
        NativeMethods.fl_string_list_dispose(handle);
        return true;
    }
}

/// <summary>An <c>fl_attachments_builder*</c>: the write end, before it is sealed.</summary>
/// <remarks>
/// Mutable and NOT thread safe, unlike every other handle in this file. That is
/// the seam's shape rather than a shortcut: a builder is scratch space a single
/// producer fills and seals, and making it shareable would mean a lock on a path
/// whose whole purpose is to cost nothing.
/// </remarks>
internal sealed class AttachmentsBuilderHandle : FletcherHandle
{
    /// <inheritdoc/>
    protected override bool ReleaseHandle()
    {
        NativeMethods.fl_attachments_builder_dispose(handle);
        return true;
    }
}

/// <summary>An <c>fl_attachments*</c>: a SEALED set, immutable and shareable.</summary>
/// <remarks>
/// The set owns one reference to each of its blobs' control blocks, built at seal
/// time. A blob handed out by an accessor is BORROWED from the set and stays
/// valid only as long as the set does - which is why this handle is what a
/// publish borrows, and why releasing it is the caller's to time.
/// </remarks>
internal sealed class AttachmentsHandle : FletcherHandle
{
    /// <inheritdoc/>
    protected override bool ReleaseHandle()
    {
        NativeMethods.fl_attachments_dispose(handle);
        return true;
    }
}

/// <summary>An <c>fl_subscriber*</c>, which keeps its provider alive.</summary>
/// <remarks>
/// The same structural pinning <see cref="PublisherHandle"/> uses, for the same
/// reason: the header says a subscriber BORROWS its provider and the provider
/// must outlive it, and reachability does not order finalization.
///
/// ── What this handle deliberately does NOT protect ──────────────────────────
/// Destroying a subscriber requires QUIESCENCE, and from inside a delivery on
/// this same subscriber the seam's answer is process termination rather than a
/// refusal - by design, because the alternative is to leak the transport
/// subscription silently. A finalizer running on the finalizer thread cannot
/// honour that clause, and nothing here pretends to: the refusal a caller can act
/// on belongs in managed code, before the call reaches this handle (D-BIND-18).
/// </remarks>
internal sealed partial class SubscriberHandle : FletcherHandle
{
    private ProviderHandle? _provider;
    private bool _addedRef;

    /// <summary>The import is PRIVATE so a subscriber cannot be made unpinned.</summary>
    [LibraryImport(NativeMethods.LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static partial int fl_subscriber_create(
        ProviderHandle provider, out SubscriberHandle subscriber, ref FlError err);

    /// <summary>Create a subscriber over a provider, and pin the provider to it.</summary>
    internal static int Create(ProviderHandle provider, out SubscriberHandle subscriber, ref FlError err)
    {
        int status = fl_subscriber_create(provider, out subscriber, ref err);
        if (status == 0 && !subscriber.IsInvalid)
        {
            subscriber.Pin(provider);
        }

        return status;
    }

    private void Pin(ProviderHandle provider)
    {
        bool added = false;
        provider.DangerousAddRef(ref added);
        _provider = provider;
        _addedRef = added;
    }

    /// <inheritdoc/>
    protected override bool ReleaseHandle()
    {
        NativeMethods.fl_subscriber_destroy(handle);

        if (_addedRef)
        {
            _provider!.DangerousRelease();
            _addedRef = false;
        }

        return true;
    }
}

/// <summary>An <c>fl_schema_arrival*</c>: a waitable handle the caller owns.</summary>
/// <remarks>
/// Disposing it does NOT cancel the subscription behind it and does not release a
/// schema already handed out - the shim holds a COPY of a copyable arrival, so
/// letting go of this end changes nothing at the other.
/// </remarks>
internal sealed class SchemaArrivalHandle : FletcherHandle
{
    /// <inheritdoc/>
    protected override bool ReleaseHandle()
    {
        NativeMethods.fl_schema_arrival_dispose(handle);
        return true;
    }
}
