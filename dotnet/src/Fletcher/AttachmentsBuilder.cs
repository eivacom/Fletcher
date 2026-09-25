// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Sidecar metadata to publish beside a row: the write end.
//
// ── The one thing this type exists to get right (D-BIND-44) ─────────────────
// `fl_attachments_builder_build` MOVES the pending entries into the sealed set
// and leaves the native builder EMPTY. The frozen public surface has no
// `Build()` and `Publisher.Publish` takes an `AttachmentsBuilder?`, so a publish
// has to seal one itself - and a publish that sealed the CALLER'S builder would
// empty it, giving row 0 its attachments and every row after it none. Silently,
// because an empty set is a legal thing to publish.
//
// So the entries live HERE, in managed memory, and the native builder is a
// transient this type repopulates. `Build` is internal, its result is cached,
// and `Set`/`Clear` drop the cache - so N publishes with unchanged attachments
// cost one seal, and a mutation between publishes is picked up rather than
// ignored.
//
// ── What is NOT provided, and why each absence is deliberate ────────────────
// No indexer, no `IDictionary`, no enumeration in key order, no LINQ. The seam
// orders keys by BYTES (memcmp), which is not C#'s ordinal UTF-16 order once a
// key leaves ASCII, so anything that let a caller iterate "in order" here would
// be handing them an order the wire does not use. Reading attachments is the
// subscriber's job and has its own positional type.
using System;
using System.Collections.Generic;
using System.Text;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>Attachments to publish with a row.</summary>
/// <remarks>
/// Not thread safe, deliberately: a builder is scratch space one producer fills.
/// Sharing one across threads is a caller error this type does not police.
/// </remarks>
public sealed class AttachmentsBuilder : IDisposable
{
    private readonly List<KeyValuePair<byte[], byte[]>> _entries = [];
    private AttachmentsHandle? _sealed;
    private bool _disposed;

    /// <summary>How many entries are pending.</summary>
    public int Count => _entries.Count;

    /// <summary>Add an entry, or replace the value of one already present.</summary>
    /// <param name="key">The key, as bytes. Copied.</param>
    /// <param name="value">The value, as bytes. Copied.</param>
    /// <exception cref="ArgumentException">The key is empty or contains a zero byte.</exception>
    /// <remarks>
    /// BYTES, NOT STRINGS, on both sides. A key is compared by bytes everywhere
    /// below this line, so letting a caller pass a <c>string</c> would mean
    /// choosing an encoding for them and then comparing in an order the wire does
    /// not use. <see cref="Set(string, ReadOnlySpan{byte})"/> exists for the common
    /// case and says which encoding it picked.
    /// </remarks>
    public void Set(ReadOnlySpan<byte> key, ReadOnlySpan<byte> value)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (key.Length == 0)
        {
            throw new ArgumentException("an attachment key cannot be empty", nameof(key));
        }

        if (key.IndexOf((byte)0) >= 0)
        {
            // The seam refuses this, and refusing it here means the caller learns
            // at the line that built the key rather than at a publish three steps
            // later. Native re-validates regardless.
            throw new ArgumentException("an attachment key cannot contain a zero byte", nameof(key));
        }

        byte[] keyCopy = key.ToArray();
        byte[] valueCopy = value.ToArray();

        for (int i = 0; i < _entries.Count; i++)
        {
            if (_entries[i].Key.AsSpan().SequenceEqual(key))
            {
                _entries[i] = new KeyValuePair<byte[], byte[]>(_entries[i].Key, valueCopy);
                Invalidate();
                return;
            }
        }

        _entries.Add(new KeyValuePair<byte[], byte[]>(keyCopy, valueCopy));
        Invalidate();
    }

    /// <summary>Add or replace an entry whose key is UTF-8 text.</summary>
    /// <remarks>
    /// The convenience form, and it names its encoding rather than leaving it to be
    /// discovered: the key is the UTF-8 of <paramref name="key"/>. A caller whose
    /// keys are not text uses the span form.
    /// </remarks>
    public void Set(string key, ReadOnlySpan<byte> value)
    {
        ArgumentNullException.ThrowIfNull(key);
        Set(Encoding.UTF8.GetBytes(key), value);
    }

    /// <summary>Drop every entry.</summary>
    public void Clear()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        _entries.Clear();
        Invalidate();
    }

    /// <summary>Seal the entries, reusing the last seal when nothing has changed.</summary>
    /// <remarks>
    /// Internal because the surface has no <c>Build()</c>: a sealed set is a thing
    /// the publish path borrows, not a thing a caller holds. The returned handle is
    /// OWNED BY THIS BUILDER and must not be disposed by the caller - it is
    /// released when this builder is disposed, or when a mutation invalidates it.
    /// </remarks>
    internal AttachmentsHandle Build()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (_sealed is not null)
        {
            return _sealed;
        }

        FlError err = default;
        int status = NativeMethods.fl_attachments_builder_create(out AttachmentsBuilderHandle builder, ref err);
        Errors.ThrowIfFailed(status, ref err);

        using (builder)
        {
            foreach (KeyValuePair<byte[], byte[]> entry in _entries)
            {
                AddOne(builder, entry.Key, entry.Value);
            }

            err = default;
            status = NativeMethods.fl_attachments_builder_build(builder, out AttachmentsHandle result, ref err);
            Errors.ThrowIfFailed(status, ref err);
            _sealed = result;
        }

        return _sealed;
    }

    /// <summary>One entry across the boundary: make a blob, set it, release ours.</summary>
    /// <remarks>
    /// The release is NOT optional and NOT deferred. `fl_attachments_builder_set`
    /// RETAINS the blob, so after it returns the builder holds a reference of its
    /// own and the one `fl_blob_create` handed us is ours to drop. Keeping it would
    /// leak a control block per entry per seal, which is the kind of leak that
    /// never shows up in a test and shows up in a service after a week.
    /// </remarks>
    private static unsafe void AddOne(AttachmentsBuilderHandle builder, byte[] key, byte[] value)
    {
        fixed (byte* valueData = value)
        fixed (byte* keyData = key)
        {
            FlError err = default;
            int status = NativeMethods.fl_blob_create((nint)valueData, (nuint)value.Length, out FlBlob blob, ref err);
            Errors.ThrowIfFailed(status, ref err);

            try
            {
                var flKey = new FlStr { Data = (nint)keyData, Len = (nuint)key.Length };
                err = default;
                status = NativeMethods.fl_attachments_builder_set(builder, flKey, in blob, ref err);
                Errors.ThrowIfFailed(status, ref err);
            }
            finally
            {
                // A finally, so a refused key does not leak the blob that was
                // already made for it.
                NativeMethods.fl_blob_release(in blob);
            }
        }
    }

    private void Invalidate()
    {
        _sealed?.Dispose();
        _sealed = null;
    }

    /// <summary>Release the sealed set, if one is being held.</summary>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _sealed?.Dispose();
        _sealed = null;
        _entries.Clear();
    }
}
