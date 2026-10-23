// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The publish tier: what a C# application actually holds to put rows on a wire.
//
// ── The fused path is the reason this class exists ──────────────────────────
// `Publish(topic, rows, i)` hands the codec the PROVIDER'S OWN window, so the
// row's bytes are written where the transport will read them and no intermediate
// buffer ever holds the row. That is the property the copy-accounting oracle
// scores, and it is why no method on the codec returns bytes: a call that hands
// the row back has already put it somewhere other than the transport.
//
// ── Three ways to produce a row, and they are not interchangeable ───────────
//   Publish(topic, rows, i)        the fused path. Arrow in, nothing copied.
//   Publish(topic, writer, n)      a producer that writes its own bytes into the
//                                  transport's window. Also zero-copy, but the
//                                  caller owns the format.
//   PublishRaw(topic, bytes)       bytes already in hand. ONE copy, unavoidable,
//                                  because the bytes exist before the window does.
//
// ── min_bytes, and a gap in the frozen surface (D-BIND-45) ──────────────────
// `fl_publisher_publish_raw` needs a `min_bytes`: the window is made big enough
// for it before the writer is called, and a `min_bytes` of 0 is refused. The
// public-surface document froze `Publish(TopicPath, RowWriter)` with nowhere to
// put that number, and it cannot be defaulted honestly - a default that is too
// small hands the writer a window it cannot fill, and the writer has no way to
// ask for more (`room` is whatever the window has, and reporting more than
// `room` is a refusal). So the RowWriter overload takes it explicitly: a
// caller stating the room they need is the only form that is not a guess.
//
// Unlike D-BIND-44 this CHANGES the public surface rather than only the
// implementation behind it, so the public-surface document is amended in the
// same commit - its 2.4 row and its class diagram both. A frozen table that
// disagrees with the shipped API is worse than no table: the next reader cannot
// tell which is current, and the table is the one they will trust.
using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Text;

using Apache.Arrow;
using Apache.Arrow.C;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>Writes one row's bytes into the transport's own window.</summary>
/// <param name="destination">
/// The window's remaining room, borrowed FOR THIS CALL ONLY. Storing it past the
/// return is a use-after-free.
/// </param>
/// <returns>
/// The number of bytes written - never a character count, and never more than
/// <paramref name="destination"/> holds.
/// </returns>
/// <remarks>
/// A writer that throws commits nothing: the thunk records the exception, the
/// publish fails, and the original exception is rethrown with its own stack
/// (D-BIND-19 rule 3).
/// </remarks>
public delegate int RowWriter(Span<byte> destination);

/// <summary>Publishes rows to topics over one provider.</summary>
public sealed unsafe class Publisher : IDisposable
{
    private readonly PublisherHandle _handle;
    private bool _disposed;

    /// <summary>Create a publisher over <paramref name="provider"/>.</summary>
    /// <remarks>
    /// The provider is BORROWED and must outlive this publisher. That is enforced
    /// structurally rather than by this comment: <see cref="PublisherHandle.Create"/>
    /// takes a SafeHandle reference on the provider, so the provider's native
    /// release cannot run while this publisher is alive, whatever order the two
    /// become garbage in.
    /// </remarks>
    public Publisher(PubSubProviderHandle provider)
    {
        ArgumentNullException.ThrowIfNull(provider);

        FlError err = default;
        int status = PublisherHandle.Create(provider.Handle, out PublisherHandle handle, ref err);
        Errors.ThrowIfFailed(status, ref err);
        _handle = handle;
    }

    /// <summary>Declare a topic and the schema its rows carry.</summary>
    /// <remarks>
    /// The schema is DEEP-COPIED by the shim, so the export made here is released
    /// as soon as the call returns. N-5: the C Data Interface's own release and the
    /// seam's owner-handle protocol are two different lifetimes, and this is one of
    /// the two places they meet.
    /// </remarks>
    public void CreateTopic(TopicPath topic, Schema schema) => CreateTopic(topic, schema, null);

    /// <summary>Declare a topic with per-topic options (D-BIND-57).</summary>
    /// <remarks>
    /// See <see cref="TopicOptions"/>: a re-declaration may repeat or omit a
    /// stored field but never change one, and a field the provider has no notion
    /// of is <see cref="FletcherStatus.NotSupported"/>. The two-argument form is
    /// this one with null options, which mean empty.
    /// </remarks>
    public void CreateTopic(TopicPath topic, Schema schema, TopicOptions? options)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(schema);

        byte* buffer = stackalloc byte[TopicPath.MaxJoinedBytes];
        FlStr* segments = stackalloc FlStr[topic.Segments.Count];
        FlTopic native = MarshalTopic(topic, buffer, segments);

        CArrowSchema* exported = CArrowSchema.Create();
        try
        {
            CArrowSchemaExporter.ExportSchema(schema, exported);

            byte[] profile = options?.Profile is { Length: > 0 } p ? Encoding.UTF8.GetBytes(p) : [];
            fixed (byte* profileBytes = profile)
            {
                var nativeOptions = new FlTopicOptions
                {
                    Profile = new FlStr { Data = (nint)profileBytes, Len = (nuint)profile.Length },
                    MaxPayloadBytes = options?.MaxPayloadBytes ?? 0,
                };

                FlError err = default;
                int status = NativeMethods.fl_publisher_create_topic_with_options(
                    _handle, native, (nint)exported, in nativeOptions, ref err);
                Errors.ThrowIfFailed(status, ref err);
            }
        }
        finally
        {
            CArrowSchema.Free(exported);
        }
    }

    /// <summary>Publish one row of a bound batch. THE FUSED PATH.</summary>
    /// <remarks>
    /// The codec runs inside the seam's publish, writing into the provider's
    /// window, so the row's bytes never exist anywhere else.
    /// </remarks>
    public void Publish(TopicPath topic, BoundRows rows, int row, AttachmentsBuilder? attachments = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(rows);
        rows.ThrowIfRowOutOfRange(row, nameof(row));

        byte* buffer = stackalloc byte[TopicPath.MaxJoinedBytes];
        FlStr* segments = stackalloc FlStr[topic.Segments.Count];
        FlTopic native = MarshalTopic(topic, buffer, segments);

        using var borrowed = new BorrowedAttachments(attachments);

        FlError err = default;
        int status = NativeMethods.fl_publisher_publish_row(
            _handle, native, rows.Handle, row, borrowed.Pointer, ref err);
        Errors.ThrowIfFailed(status, ref err);
    }

    /// <summary>Publish every row of a bound batch, in ONE crossing.</summary>
    /// <remarks>
    /// <para>
    /// The topic is converted once rather than per row, which is the only reason
    /// this is not a loop the caller could have written - and it is why the ABI
    /// takes a range.
    /// </para>
    /// <para>
    /// PARTIAL PUBLICATION IS NOT UNWOUND, and cannot be: rows already handed to
    /// the transport have gone out. A failure at row k means rows [0, k) were
    /// published and the rest were not, which is a resend decision rather than a
    /// rollback.
    /// </para>
    /// </remarks>
    public void Publish(TopicPath topic, BoundRows rows, AttachmentsBuilder[]? attachmentsPerRow = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(rows);

        if (attachmentsPerRow is not null && attachmentsPerRow.Length != rows.Length)
        {
            throw new ArgumentException(
                $"attachmentsPerRow has {attachmentsPerRow.Length} entries for a batch of {rows.Length} rows; " +
                "it must have exactly one entry per row, or be null",
                nameof(attachmentsPerRow));
        }

        byte* buffer = stackalloc byte[TopicPath.MaxJoinedBytes];
        FlStr* segments = stackalloc FlStr[topic.Segments.Count];
        FlTopic native = MarshalTopic(topic, buffer, segments);

        if (attachmentsPerRow is null)
        {
            FlError plain = default;
            int plainStatus = NativeMethods.fl_publisher_publish_rows(
                _handle, native, rows.Handle, 0, rows.Length, 0, ref plain);
            Errors.ThrowIfFailed(plainStatus, ref plain);
            return;
        }

        // One sealed set per row, each borrowed for the whole crossing. Sealing is
        // done UP FRONT rather than inside the loop native runs, because native
        // runs no managed code here - the pointers must all be live before the
        // call starts.
        var borrowed = new BorrowedAttachments[attachmentsPerRow.Length];
        nint[] pointers = new nint[attachmentsPerRow.Length];
        try
        {
            for (int i = 0; i < attachmentsPerRow.Length; i++)
            {
                borrowed[i] = new BorrowedAttachments(attachmentsPerRow[i]);
                pointers[i] = borrowed[i].Pointer;
            }

            fixed (nint* table = pointers)
            {
                FlError err = default;
                int status = NativeMethods.fl_publisher_publish_rows(
                    _handle, native, rows.Handle, 0, rows.Length, (nint)table, ref err);
                Errors.ThrowIfFailed(status, ref err);
            }
        }
        finally
        {
            foreach (BorrowedAttachments one in borrowed)
            {
                one.Dispose();
            }
        }
    }

    /// <summary>Publish bytes a producer writes straight into the transport window.</summary>
    /// <param name="topic">The topic to publish to.</param>
    /// <param name="writer">The producer. Invoked exactly once, inside the publish.</param>
    /// <param name="minBytes">
    /// The room the writer needs, at least. Refused if zero. See this file's header
    /// for why this is a parameter rather than a default.
    /// </param>
    /// <param name="attachments">Sidecar metadata, or null.</param>
    public void Publish(TopicPath topic, RowWriter writer, int minBytes, AttachmentsBuilder? attachments = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(writer);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(minBytes);

        PublishThroughWriter(topic, writer, minBytes, attachments);
    }

    /// <summary>Publish bytes already in hand.</summary>
    /// <remarks>
    /// ONE COPY, and it is unavoidable rather than an oversight: the bytes exist
    /// before the transport's window does, so something has to move them into it.
    /// A caller who can produce bytes on demand uses the
    /// <see cref="Publish(TopicPath, RowWriter, int, AttachmentsBuilder?)"/> form
    /// and pays none.
    /// </remarks>
    public void PublishRaw(TopicPath topic, ReadOnlySpan<byte> row, AttachmentsBuilder? attachments = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (row.Length == 0)
        {
            // The seam refuses a min_bytes of 0 - "a fill of no bytes names
            // nothing" - so this would be refused natively anyway. Refused here so
            // the message names the argument rather than the ABI parameter.
            throw new ArgumentException("a published row must carry at least one byte", nameof(row));
        }

        fixed (byte* source = row)
        {
            var copier = new SpanCopier { Source = source, Length = row.Length };
            PublishThroughWriter(topic, copier.Write, row.Length, attachments);
        }
    }

    /// <summary>The topics this publisher has declared, as their joined names.</summary>
    public IReadOnlyList<string> ListTopics()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        FlError err = default;
        int status = NativeMethods.fl_publisher_list_topics(_handle, out StringListHandle list, ref err);
        Errors.ThrowIfFailed(status, ref err);

        using (list)
        {
            nuint count = NativeMethods.fl_string_list_size(list);
            var names = new string[count];
            for (nuint i = 0; i < count; i++)
            {
                FlStr name = NativeMethods.fl_string_list_at(list, i);
                names[i] = name.Data == 0
                    ? string.Empty
                    : Encoding.UTF8.GetString((byte*)name.Data, checked((int)name.Len));
            }

            return names;
        }
    }

    /// <summary>Release the publisher.</summary>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _handle.Dispose();
    }

    /// <summary>The shared body of both writer-driven publishes.</summary>
    private void PublishThroughWriter(TopicPath topic, RowWriter writer, int minBytes, AttachmentsBuilder? attachments)
    {
        byte* buffer = stackalloc byte[TopicPath.MaxJoinedBytes];
        FlStr* segments = stackalloc FlStr[topic.Segments.Count];
        FlTopic native = MarshalTopic(topic, buffer, segments);

        var state = new WriterState { Writer = writer };
        GCHandle token = GCHandle.Alloc(state);
        using var borrowed = new BorrowedAttachments(attachments);

        try
        {
            FlError err = default;
            int status = NativeMethods.fl_publisher_publish_raw(
                _handle,
                native,
                (nint)(delegate* unmanaged[Cdecl]<nint, byte*, nuint, nuint>)&WriterState.Thunk,
                GCHandle.ToIntPtr(token),
                (nuint)minBytes,
                borrowed.Pointer,
                ref err);

            // The captured exception outranks the status the shim synthesised from
            // it, which is what ThrowIfFailed's first rule is for.
            Errors.ThrowIfFailed(status, ref err, state.Captured);
        }
        finally
        {
            token.Free();
        }
    }

    /// <summary>A topic's segments, laid into one stack buffer.</summary>
    /// <remarks>
    /// ONE buffer and one pointer array, both stack-allocated, both bounded: the
    /// joined name cannot exceed 246 bytes, so neither can the sum of the segments,
    /// and a segment is at least one byte so there cannot be more than 246 of them.
    /// The bound is the seam's own rule doing double duty - without it this would
    /// need a heap allocation on the publish path.
    /// </remarks>
    private static FlTopic MarshalTopic(TopicPath topic, byte* buffer, FlStr* segments)
    {
        byte[][] utf8 = topic.Utf8Segments;
        int offset = 0;

        for (int i = 0; i < utf8.Length; i++)
        {
            byte[] segment = utf8[i];
            segment.AsSpan().CopyTo(new Span<byte>(buffer + offset, segment.Length));
            segments[i] = new FlStr { Data = (nint)(buffer + offset), Len = (nuint)segment.Length };
            offset += segment.Length;
        }

        return new FlTopic { Segments = (nint)segments, Count = (nuint)utf8.Length };
    }

    /// <summary>A sealed attachments set, kept alive for exactly one crossing.</summary>
    /// <remarks>
    /// <c>DangerousGetHandle</c> alone would not do: the raw pointer does not keep
    /// the SafeHandle reachable, so the collector could finalize the set while
    /// native code is still reading it. The AddRef/Release pair is the documented
    /// way to hold one across a call that receives it as a plain pointer.
    /// </remarks>
    private readonly struct BorrowedAttachments : IDisposable
    {
        private readonly AttachmentsHandle? _handle;
        private readonly bool _addedRef;

        internal BorrowedAttachments(AttachmentsBuilder? builder)
        {
            _handle = builder?.Build();
            _addedRef = false;
            Pointer = 0;

            if (_handle is null)
            {
                return;
            }

            bool added = false;
            _handle.DangerousAddRef(ref added);
            _addedRef = added;
            Pointer = _handle.DangerousGetHandle();
        }

        /// <summary>The <c>fl_attachments*</c>, or 0 for none.</summary>
        internal nint Pointer { get; }

        /// <inheritdoc/>
        public void Dispose()
        {
            if (_addedRef)
            {
                _handle!.DangerousRelease();
            }
        }
    }

    /// <summary>Copies a span the caller already holds. Used by PublishRaw.</summary>
    /// <remarks>
    /// A class with a pointer field rather than a lambda, because a lambda cannot
    /// capture a pointer local - and the pointer is what keeps this to one copy
    /// instead of two.
    /// </remarks>
    private sealed class SpanCopier
    {
        internal byte* Source;
        internal int Length;

        internal int Write(Span<byte> destination)
        {
            new ReadOnlySpan<byte>(Source, Length).CopyTo(destination);
            return Length;
        }
    }

    /// <summary>The managed side of an <c>fl_writer_fn</c>.</summary>
    private sealed class WriterState
    {
        internal RowWriter Writer = null!;

        internal ExceptionDispatchInfo? Captured { get; private set; }

        /// <summary>The writer hook native calls. NOTHING may escape it.</summary>
        /// <remarks>
        /// An exception leaving an <c>[UnmanagedCallersOnly]</c> method is a
        /// fail-fast, not an exception: the runtime cannot unwind through a C
        /// frame. So this records and returns 0, which the header defines as the
        /// binding's signal that its thunk captured something; the shim then
        /// reports FL_ORIGIN_CALLBACK and <c>Errors.ThrowIfFailed</c> rethrows the
        /// original with its own stack.
        /// </remarks>
        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        internal static nuint Thunk(nint ctx, byte* destination, nuint room)
        {
            WriterState? state = null;
            try
            {
                state = (WriterState?)GCHandle.FromIntPtr(ctx).Target;

                // A span cannot be longer than int.MaxValue. Clamping rather than
                // refusing: the writer still gets a valid window far larger than
                // any row, and a refusal here would be this binding inventing a
                // limit the ABI does not have.
                int usable = room > int.MaxValue ? int.MaxValue : (int)room;
                int written = state!.Writer(new Span<byte>(destination, usable));

                if (written < 0 || written > usable)
                {
                    throw new InvalidOperationException(
                        $"a RowWriter reported {written} bytes written into a {usable}-byte window");
                }

                if (written == 0)
                {
                    // 0 is RESERVED as the thunk's own failure signal, so a writer
                    // returning it honestly would be indistinguishable from one
                    // that threw. Converted into a real exception, which is at
                    // least a message the caller can act on.
                    throw new InvalidOperationException(
                        "a RowWriter must write at least one byte: 0 is reserved for signalling a failed writer");
                }

                return (nuint)written;
            }
            catch (Exception exception)
            {
                try
                {
                    if (state is not null)
                    {
                        state.Captured = ExceptionDispatchInfo.Capture(exception);
                    }
                }
                catch (Exception)
                {
                    // Recording the failure failed too. The publish still fails,
                    // carrying the shim's own text. What must NOT happen here is a
                    // second exception leaving this frame.
                }

                return 0;
            }
        }
    }
}
