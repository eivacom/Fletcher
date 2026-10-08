// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// One Arrow batch, validated once and then used N times.
//
// ── Why a batch has a type at all ──────────────────────────────────────────
// The three-step shape (open, bind, encode) exists because each step has a
// different cost and a different lifetime: opening reads a schema, binding
// validates every buffer in a batch, and encoding walks one row. Collapsing bind
// into encode would re-validate the whole batch per row, and collapsing it into
// open would tie a codec to one batch.
//
// So `BoundRows` is what the middle step returns, and it owns two things that
// must die in a fixed order - the native view, and the export the view borrows.
// That order is enforced by `BoundRowsHandle` rather than by the body of
// `Dispose` below, which is what makes it survive a caller who never calls it.
using System;

using Apache.Arrow;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>One Arrow record batch bound to a codec, ready to encode from.</summary>
/// <remarks>
/// Immutable after construction, like the codec itself: N threads may encode or
/// publish different rows of one bound batch concurrently without a lock.
///
/// The batch's data is BORROWED. Disposing this releases the export that keeps
/// that data alive from native's point of view, so no row of it may be encoded
/// afterwards.
/// </remarks>
public sealed class BoundRows : IDisposable
{
    private readonly BoundRowsHandle _handle;
    private readonly ExportedArray _export;
    private bool _disposed;

    internal BoundRows(FletcherCodec codec, BoundRowsHandle handle, ExportedArray export, int length)
    {
        Codec = codec;
        _handle = handle;
        _export = export;
        Length = length;
    }

    /// <summary>How many rows this batch carries.</summary>
    public int Length { get; }

    /// <summary>The schema these rows were validated against.</summary>
    public Schema Schema => Codec.Schema;

    /// <summary>The codec this batch is bound to.</summary>
    /// <remarks>
    /// Kept so that <see cref="FletcherCodec.Encode"/> can refuse a batch bound to
    /// a DIFFERENT codec. Native would not catch that: an `fl_rows` carries its
    /// own codec, so the encode would succeed and silently produce bytes for the
    /// other schema.
    /// </remarks>
    internal FletcherCodec Codec { get; }

    /// <summary>The native view, for the encode and publish entry points.</summary>
    internal BoundRowsHandle Handle => _handle;

    /// <summary>The export, whose intactness is the borrow rule made observable.</summary>
    internal ExportedArray Export => _export;

    /// <summary>Unbind the view, then release the exported array.</summary>
    /// <remarks>
    /// In that order, and not by convention: the handle holds a reference on the
    /// export, so the release below cannot take effect until the unbind has run —
    /// including on the finalizer thread, if this is never called at all.
    /// </remarks>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _handle.Dispose();
        _export.Dispose();
    }

    /// <summary>Refuse a row index outside the batch, before it reaches native.</summary>
    /// <remarks>
    /// Native refuses it too, but it can only answer with a status and a message;
    /// this says which argument was wrong, which is what a managed caller's own
    /// contract promises.
    /// </remarks>
    internal void ThrowIfRowOutOfRange(int row, string parameterName)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if ((uint)row >= (uint)Length)
        {
            throw new ArgumentOutOfRangeException(
                parameterName, row, $"the bound batch has {Length} rows");
        }
    }
}
