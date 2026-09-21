// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The ABI's by-value types, mirrored field for field.
//
// ── Why every one of these is blittable, and why that is not an accident ────
// `LibraryImport` refuses to marshal a non-blittable type without being told
// how, which means the compiler checks the claim these structs make: that the
// managed layout and the C layout are the same bytes. A `bool` field, a `char`,
// or a `string` would each turn a compile-time guarantee into a runtime
// conversion, and a conversion is a copy with an opinion. So pointers are `nint`
// and `size_t` is `nuint` throughout, and the interpretation - which pointer is
// owned, which is borrowed, what the length counts - lives in the header and in
// the tier above, not here.
//
// `size_t` is `nuint` rather than `ulong` deliberately: the two differ on a
// 32-bit target, and while the round ships x64 only (D-BIND-27), a type that is
// wrong on a platform nobody builds today is a defect waiting for the day
// somebody does.
using System.Runtime.InteropServices;

namespace Eiva.Fletcher.Interop;

/// <summary>Bytes plus a length, BORROWED for the duration of a call (`fl_str`).</summary>
/// <remarks>
/// The length is authoritative and the bytes are not NUL-terminated: a zero byte
/// inside a topic segment or a configuration document is data and survives the
/// crossing. That is the whole reason the ABI does not use C strings.
/// </remarks>
[StructLayout(LayoutKind.Sequential)]
internal struct FlStr
{
    /// <summary>`const uint8_t* data`.</summary>
    internal nint Data;

    /// <summary>`size_t len`.</summary>
    internal nuint Len;
}

/// <summary>A topic as an ordered list of segments (`fl_topic`).</summary>
/// <remarks>
/// Both the array and every segment in it are borrowed for the call. The shim
/// does not re-validate the six topic rules; it inherits them by passing every
/// path through the seam's own `RequireSegments`, so a malformed topic comes back
/// carrying the seam's message rather than a second taxonomy's.
/// </remarks>
[StructLayout(LayoutKind.Sequential)]
internal struct FlTopic
{
    /// <summary>`const fl_str* segments`.</summary>
    internal nint Segments;

    /// <summary>`size_t count`.</summary>
    internal nuint Count;
}

/// <summary>A failure carried by value to the caller (`fl_error`).</summary>
/// <remarks>
/// Zero-initialise before every fallible call: on success the callee leaves it
/// untouched. On failure the caller owns <see cref="Message"/> and releases it
/// with <c>fl_error_dispose</c>, which is why the managed throw site is a
/// <c>finally</c>.
///
/// The reverse direction exists too and is governed by D-BIND-32: an
/// <c>fl_error</c> that a CALLBACK fills is BORROWED to the shim, which copies
/// what it needs and frees nothing. A managed grow or writer thunk that sets
/// <see cref="Message"/> must therefore keep those bytes alive until it returns,
/// and must never dispose one of its own.
/// </remarks>
[StructLayout(LayoutKind.Sequential)]
internal struct FlError
{
    /// <summary>`int32_t status` — an `fl_status` value, or 0 for FL_OK.</summary>
    internal int Status;

    /// <summary>`int32_t origin` — which containment site produced it.</summary>
    internal int Origin;

    /// <summary>`uint8_t* message` — UTF-8, NOT NUL-terminated, NULL on success.</summary>
    internal nint Message;

    /// <summary>`size_t message_len`.</summary>
    internal nuint MessageLen;
}

/// <summary>Bytes plus what keeps them alive (`fl_blob`).</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct FlBlob
{
    /// <summary>`void* owner` — opaque; NULL only when <see cref="Size"/> is 0.</summary>
    internal nint Owner;

    /// <summary>`const uint8_t* data` — NULL when <see cref="Size"/> is 0.</summary>
    internal nint Data;

    /// <summary>`size_t size`.</summary>
    internal nuint Size;
}

/// <summary>The typed core of a provider configuration (`fl_provider_config`).</summary>
/// <remarks>
/// <see cref="Document"/> crosses VERBATIM and is never parsed by Fletcher — the
/// length is authoritative and the bytes reach the provider whole.
/// </remarks>
[StructLayout(LayoutKind.Sequential)]
internal struct FlProviderConfig
{
    /// <summary>`uint32_t max_payload_bytes`.</summary>
    internal uint MaxPayloadBytes;

    /// <summary>`uint32_t domain_id`.</summary>
    internal uint DomainId;

    /// <summary>`fl_str document`.</summary>
    internal FlStr Document;
}

/// <summary>
/// The C form of the seam's write buffer: a window plus a refill hook
/// (`fl_write_window`).
/// </summary>
/// <remarks>
/// A WINDOW MEANS ONE CROSSING PER REFILL, NOT ONE PER APPEND — that is the whole
/// reason the type exists rather than an append-a-byte function. The normative
/// properties a binding must honour when writing into one are on `fl_write_window`
/// in the header: random access rather than a stream, bounds computed by
/// SUBTRACTION so a hostile length cannot wrap, <see cref="Data"/> readable only
/// over [Data, Data + Pos), and a NULL <see cref="Grow"/> meaning fixed capacity.
/// </remarks>
[StructLayout(LayoutKind.Sequential)]
internal struct FlWriteWindow
{
    /// <summary>`void* ctx` — the producer's own state; Fletcher never reads it.</summary>
    internal nint Ctx;

    /// <summary>`uint8_t* data` — the window base.</summary>
    internal nint Data;

    /// <summary>`size_t capacity`.</summary>
    internal nuint Capacity;

    /// <summary>`size_t pos` — the write cursor is Data + Pos.</summary>
    internal nuint Pos;

    /// <summary>
    /// `fl_grow_fn grow` — NULL for a fixed-capacity window.
    /// </summary>
    /// <remarks>
    /// Held as a raw pointer rather than a delegate type: it is set from an
    /// <c>[UnmanagedCallersOnly]</c> function pointer, which is what keeps the
    /// refill path free of a marshalling stub and of the GC handle a delegate
    /// would need to stay alive across the call.
    /// </remarks>
    internal nint Grow;
}
