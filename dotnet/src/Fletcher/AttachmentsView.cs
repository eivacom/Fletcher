// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Reading the attachments a delivery carried: positional, borrowed, and a ref
// struct on purpose.
//
// ── Why a ref struct (N-3) ──────────────────────────────────────────────────
// The set this views is BORROWED for the duration of one delivery callback and
// belongs to the shim. A ref struct cannot be stored in a field, boxed, captured
// by a lambda, or held across an await - so the compiler refuses the shapes that
// would let it outlive the call. That also means an `async` handler CANNOT
// COMPILE against this type, which is the point rather than a side effect: an
// async handler would return at its first await while the transport thread
// carried on and freed everything it was still reading.
//
// A caller who wants the bytes past the callback copies them, or retains the
// blob. Both are explicit, and that is the whole design.
//
// ── Positional, and never sorted (D-BIND-20) ────────────────────────────────
// Entries come back in the order Fletcher publishes them, which is `memcmp`
// order over the UTF-8 keys. That is NOT C#'s ordinal UTF-16 order - U+FF5E
// sorts before U+10000 in one and after it in the other - so this type offers no
// comparer, no IDictionary, no LINQ ordering and no sort. Anything that let a
// caller iterate "in order" here would be handing them an order the wire does
// not use.
using System;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>The attachments a delivery carried. Valid only inside the callback.</summary>
public readonly ref struct AttachmentsView
{
    private readonly nint _attachments;

    internal AttachmentsView(nint attachments) => _attachments = attachments;

    /// <summary>How many entries the delivery carried.</summary>
    public int Count => _attachments == 0
        ? 0
        : checked((int)NativeMethods.fl_attachments_size_raw(_attachments));

    /// <summary>The key at <paramref name="index"/>, as bytes.</summary>
    /// <remarks>
    /// BYTES, not a string. A key is compared by bytes everywhere below this line,
    /// and decoding one to UTF-16 here would invite comparisons in an order the
    /// wire does not use. Out of range yields an empty span rather than throwing,
    /// matching the ABI accessor it wraps: the caller learns the size from
    /// <see cref="Count"/>, and there is nowhere in the signature to put a status.
    /// </remarks>
    public unsafe ReadOnlySpan<byte> KeyAt(int index)
    {
        if (_attachments == 0 || index < 0 || index >= Count)
        {
            return default;
        }

        FlStr key = NativeMethods.fl_attachments_key_at_raw(_attachments, (nuint)index);
        return key.Data == 0 ? default : new ReadOnlySpan<byte>((void*)key.Data, checked((int)key.Len));
    }

    /// <summary>The value at <paramref name="index"/>, as bytes.</summary>
    /// <remarks>
    /// BORROWED from the set, like everything else here. An entry with an empty
    /// value is a real entry whose value has no bytes - the key is the signal -
    /// and it comes back as an empty span, which is also what an out-of-range
    /// index gives. Use <see cref="Count"/> to tell the two apart.
    /// </remarks>
    public unsafe ReadOnlySpan<byte> ValueAt(int index)
    {
        if (_attachments == 0 || index < 0 || index >= Count)
        {
            return default;
        }

        FlBlob value = NativeMethods.fl_attachments_value_at_raw(_attachments, (nuint)index);
        return value.Data == 0 ? default : new ReadOnlySpan<byte>((void*)value.Data, checked((int)value.Size));
    }

    /// <summary>Look an entry up by key.</summary>
    /// <returns>Whether the key was present.</returns>
    /// <remarks>
    /// Absence is an ordinary answer rather than a failure, which is why this
    /// returns a bool and not a status: a caller forced to distinguish "no such
    /// key" from a refusal would need a taxonomy for something that is not one.
    /// </remarks>
    public unsafe bool TryFind(ReadOnlySpan<byte> key, out ReadOnlySpan<byte> value)
    {
        value = default;

        if (_attachments == 0 || key.Length == 0)
        {
            return false;
        }

        fixed (byte* keyData = key)
        {
            var flKey = new FlStr { Data = (nint)keyData, Len = (nuint)key.Length };
            if (NativeMethods.fl_attachments_find(_attachments, flKey, out FlBlob blob) == 0)
            {
                return false;
            }

            value = blob.Data == 0
                ? default
                : new ReadOnlySpan<byte>((void*)blob.Data, checked((int)blob.Size));
            return true;
        }
    }
}
