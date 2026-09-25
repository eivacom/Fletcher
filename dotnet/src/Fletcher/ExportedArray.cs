// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The managed side of the borrow rule: an exported `ArrowArray` as a handle.
//
// ── Why this is a SafeHandle and not a field on BoundRows ───────────────────
// `fl_rows_bind` BORROWS the array and never consumes it, so the export stays
// the caller's to release - and the header asks a binding to make the ORDER
// structural rather than remembered: unbind first, release afterwards.
//
// A plain field would express that order only in the body of `Dispose`, which is
// the one path a forgotten `using` skips. Making the export a SafeHandle of its
// own puts it under the finalizer, and letting `BoundRowsHandle` take a
// reference on it means the release cannot run before the unbind even when both
// become garbage in the same collection and are finalized in an order nobody
// chose. That is the same argument `Handles.cs` makes for a publisher and its
// provider, applied to the pair one tier up.
using System;
using System.Runtime.InteropServices;

using Apache.Arrow;
using Apache.Arrow.C;

namespace Eiva.Fletcher;

/// <summary>A <c>CArrowArray*</c> this process exported and still owns.</summary>
/// <remarks>
/// Releasing means two distinct things that are easy to conflate: calling the C
/// Data Interface <c>release</c> callback, which tears down what the export
/// pinned, and freeing the structure the pointer points at. Both happen here, in
/// that order.
/// </remarks>
internal sealed unsafe class ExportedArray : SafeHandle
{
    private ExportedArray()
        : base(IntPtr.Zero, ownsHandle: true)
    {
    }

    /// <inheritdoc/>
    public override bool IsInvalid => handle == IntPtr.Zero;

    /// <summary>Export <paramref name="batch"/> as a struct array the ABI can bind.</summary>
    /// <remarks>
    /// A record batch exports as a STRUCT array whose children are its columns,
    /// which is precisely the shape <c>fl_rows_bind</c> validates against the
    /// codec's schema. The structure is claimed by this handle before the export
    /// runs, so an export that throws still leaves the allocation owned by
    /// something that will free it.
    /// </remarks>
    internal static ExportedArray Export(RecordBatch batch)
    {
        ExportedArray self = new();
        self.SetHandle((nint)CArrowArray.Create());
        CArrowArrayExporter.ExportRecordBatch(batch, (CArrowArray*)self.handle);
        return self;
    }

    /// <summary>Is the export still unconsumed?</summary>
    /// <remarks>
    /// The observable form of the borrow rule, and the managed twin of the C++
    /// suite's <c>"fl_rows_bind consumed the caller's array"</c> assertion: a
    /// <c>release</c> that is still non-NULL is what "the callee did not take it"
    /// looks like from this side.
    /// </remarks>
    internal bool IsIntact => !IsInvalid && !IsClosed && ((ArrowArrayAbi*)handle)->Release != 0;

    /// <inheritdoc/>
    protected override bool ReleaseHandle()
    {
        ArrowArrayAbi* array = (ArrowArrayAbi*)handle;

        // Called explicitly rather than left to `Free`. `CArrowArray.release` is
        // internal to Apache.Arrow, so whether `Free` invokes it is not something
        // this assembly can read - and the difference between the two readings is
        // a leaked export on every bind. The C Data Interface requires a release
        // callback to NULL itself, so doing it here is safe under both: a `Free`
        // that also checks finds nothing left to do.
        if (array->Release != 0)
        {
            ((delegate* unmanaged[Cdecl]<ArrowArrayAbi*, void>)array->Release)(array);
        }

        CArrowArray.Free((CArrowArray*)handle);
        return true;
    }
}

/// <summary>The Arrow C Data Interface's <c>ArrowArray</c>, laid out by the spec.</summary>
/// <remarks>
/// Mirrored here for one field. <c>Apache.Arrow</c>'s own <c>CArrowArray</c> keeps
/// <c>release</c> internal, and this assembly needs to both read it (the borrow
/// rule is only observable through it) and call it (see
/// <see cref="ExportedArray.ReleaseHandle"/>).
///
/// The layout is not a guess and cannot drift: <c>arrow/c/abi.h</c> is a frozen
/// specification whose field order is the interchange contract itself, which is
/// the same reason the shim can be handed a structure Apache.Arrow allocated.
/// </remarks>
[StructLayout(LayoutKind.Sequential)]
internal struct ArrowArrayAbi
{
    /// <summary>`int64_t length`.</summary>
    internal long Length;

    /// <summary>`int64_t null_count`.</summary>
    internal long NullCount;

    /// <summary>`int64_t offset`.</summary>
    internal long Offset;

    /// <summary>`int64_t n_buffers`.</summary>
    internal long BufferCount;

    /// <summary>`int64_t n_children`.</summary>
    internal long ChildCount;

    /// <summary>`const void** buffers`.</summary>
    internal nint Buffers;

    /// <summary>`struct ArrowArray** children`.</summary>
    internal nint Children;

    /// <summary>`struct ArrowArray* dictionary`.</summary>
    internal nint Dictionary;

    /// <summary>`void (*release)(struct ArrowArray*)` — NULL once consumed.</summary>
    internal nint Release;

    /// <summary>`void* private_data`.</summary>
    internal nint PrivateData;
}
