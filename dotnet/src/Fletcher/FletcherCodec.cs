// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The codec tier: one wire format, running natively, reached from C#.
//
// ── The one design decision this file exists to carry ───────────────────────
// D-BIND-1: there is ONE codec, and it is the C++ one. Managed code does not
// write wire bytes and this class does not either - it hands Arrow arrays across
// the C Data Interface and lets the shim's `positional_io` do the encoding, so
// that a C# publisher and a C++ subscriber cannot disagree about the format by
// the usual route, which is two implementations drifting.
//
// That is also why NO METHOD HERE RETURNS BYTES. `Encode` writes into a buffer
// the caller supplies, because a method returning a `byte[]` would have written
// the row somewhere other than the transport's window, which is exactly the
// whole-row copy the copy-accounting oracle exists to catch. The zero-copy route
// is `Publisher.Publish(topic, rows, i)` and it lands with the pub/sub tier.
//
// ── Three steps, three lifetimes ────────────────────────────────────────────
//   open   once per schema - validates the mapping, precomputes the field plan
//   bind   once per batch  - validates every buffer, builds the array view
//   encode once per row    - walks one row
// Each is a different cost, which is the whole reason they are separate calls
// rather than one convenient `Encode(batch, row)`.
using System;
using System.Buffers;

using Apache.Arrow;
using Apache.Arrow.C;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>The Fletcher wire format over one Arrow schema.</summary>
/// <remarks>
/// Immutable after construction, so any number of threads may bind, encode and
/// decode through one instance concurrently without a lock. Disposing it is the
/// one operation that is not thread-safe against the others.
///
/// <para>
/// <b>Strings cross a transcoding boundary, and its cost is real</b> (D-BIND-1b).
/// .NET holds strings as UTF-16; the wire carries UTF-8. So a
/// <see cref="Apache.Arrow.StringArray"/> built by managed code is already UTF-8
/// in its buffers — Arrow's own representation — and passes through untouched,
/// but a string that reaches Arrow from a .NET <c>string</c> is transcoded on the
/// way in, once, by <c>Apache.Arrow</c> rather than by Fletcher.
/// </para>
/// <para>
/// Two consequences worth knowing before measuring anything. Every length on the
/// wire is a count of UTF-8 BYTES, never of characters or of UTF-16 code units,
/// so a string whose <c>Length</c> is 10 may occupy 30 bytes. And the copy
/// accounting the seam is graded on does not count that transcode: it is a
/// property of the .NET string type, not of this boundary, and it happens before
/// a row reaches the codec at all.
/// </para>
/// </remarks>
public sealed unsafe class FletcherCodec : IDisposable
{
    private readonly CodecHandle _codec;
    private bool _disposed;

    /// <summary>Open a codec over <paramref name="schema"/>.</summary>
    /// <param name="schema">The Arrow schema every row will conform to.</param>
    /// <exception cref="ArgumentNullException"><paramref name="schema"/> is null.</exception>
    /// <exception cref="FletcherFormatException">
    /// The schema contains a type the wire format does not carry. The message
    /// names the offending field.
    /// </exception>
    /// <remarks>
    /// The schema is BORROWED and deep-copied by the shim, so the export this
    /// constructor makes is released before it returns — on the failure path too.
    /// Nothing the caller does to <paramref name="schema"/> afterwards reaches the
    /// codec.
    /// </remarks>
    public FletcherCodec(Schema schema)
    {
        ArgumentNullException.ThrowIfNull(schema);
        Schema = schema;

        CArrowSchema* exported = CArrowSchema.Create();
        try
        {
            CArrowSchemaExporter.ExportSchema(schema, exported);

            FlError err = default;
            int status = NativeMethods.fl_codec_open((nint)exported, out CodecHandle codec, ref err);
            if (status != (int)FletcherStatus.Ok)
            {
                codec.Dispose();
            }

            Errors.ThrowIfFailed(status, ref err);
            _codec = codec;
        }
        finally
        {
            CArrowSchema.Free(exported);
        }

        // Derived only after the shim accepted the schema, so a schema the wire
        // format refuses is refused by the shim's message — which names the field
        // — rather than by a rewrite failing for a second reason.
        DecodedSchema = Fletcher.DecodedSchema.Resolve(Schema);
    }

    /// <summary>The schema this codec was opened over, and binds against.</summary>
    public Schema Schema { get; }

    /// <summary>The schema <see cref="Decode"/> and <see cref="DecodeBatch"/> produce.</summary>
    /// <remarks>
    /// The same object as <see cref="Schema"/> unless the schema carries a
    /// DICTIONARY. The wire format carries a dictionary field as its value type,
    /// one value per row — the indices are a columnar optimisation with no meaning
    /// in a single row — so what comes back is a plain value array (D-BIND-39,
    /// spec §"Dictionary Types"). Re-folding those values into a
    /// <c>DictionaryArray</c> belongs to the batched subscriber.
    ///
    /// Reference equality with <see cref="Schema"/> is the cheap way to ask
    /// whether this codec decodes into something else.
    /// </remarks>
    public Schema DecodedSchema { get; }

    /// <summary>Make the next refill of an encode throw this. Tests only.</summary>
    /// <remarks>
    /// Per codec rather than static, so the test classes stay parallelisable, and
    /// internal so it is not a supported knob. It exists because the thunk's
    /// containment — the rule that no exception may cross a boundary the runtime
    /// cannot unwind — is otherwise unreachable: <see cref="GrowingWindow"/> grows
    /// into a pooled buffer of its own, so a refill has nothing left to fail at.
    /// An unexecuted catch block is a claim, and this round has already been
    /// caught believing one (D-BIND-34).
    /// </remarks>
    internal Exception? RefillFaultForTest { get; set; }

    /// <summary>Bind one record batch, validating every buffer once.</summary>
    /// <param name="batch">The rows to encode from. Its data is BORROWED.</param>
    /// <returns>A handle to the bound batch; dispose it when the rows are done.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="batch"/> is null.</exception>
    /// <exception cref="FletcherException">
    /// The batch is not a struct matching this codec's schema.
    /// </exception>
    /// <remarks>
    /// ONE BIND SERVES N ENCODES, and that is the point of the call existing: the
    /// buffer validation happens here, not per row. The exported array is
    /// borrowed and never consumed by the shim, so the returned
    /// <see cref="BoundRows"/> owns it and releases it — after unbinding — when
    /// disposed.
    /// </remarks>
    public BoundRows Bind(RecordBatch batch)
    {
        ArgumentNullException.ThrowIfNull(batch);
        ObjectDisposedException.ThrowIf(_disposed, this);

        ExportedArray export = ExportedArray.Export(batch);
        try
        {
            FlError err = default;
            int status = BoundRowsHandle.Bind(_codec, export, out BoundRowsHandle rows, ref err);
            if (status != (int)FletcherStatus.Ok)
            {
                rows.Dispose();
            }

            Errors.ThrowIfFailed(status, ref err);
            return new BoundRows(this, rows, export, batch.Length);
        }
        catch
        {
            // The bind never happened, so nothing holds a reference on the export
            // and this is the only place that can release it.
            export.Dispose();
            throw;
        }
    }

    /// <summary>Encode row <paramref name="row"/> into <paramref name="output"/>.</summary>
    /// <param name="rows">A batch bound to THIS codec.</param>
    /// <param name="row">The row index within that batch.</param>
    /// <param name="output">Where the bytes go.</param>
    /// <exception cref="ArgumentNullException">An argument is null.</exception>
    /// <exception cref="ArgumentException">
    /// <paramref name="rows"/> is bound to a different codec.
    /// </exception>
    /// <exception cref="ArgumentOutOfRangeException">
    /// <paramref name="row"/> is outside the batch.
    /// </exception>
    /// <remarks>
    /// The route for bytes in hand — a write-ahead log, a test, a relay. The
    /// zero-copy route is publishing, where the codec runs inside the transport's
    /// own window and no intermediate bytes exist.
    ///
    /// A FAILED ENCODE ADVANCES NOTHING. <paramref name="output"/> is left exactly
    /// as it was found, so a caller batching rows into one writer can retry or
    /// abandon a row without a partial one in the stream.
    /// </remarks>
    public void Encode(BoundRows rows, int row, IBufferWriter<byte> output)
    {
        ArgumentNullException.ThrowIfNull(rows);
        ArgumentNullException.ThrowIfNull(output);
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (!ReferenceEquals(rows.Codec, this))
        {
            // Native cannot catch this: an `fl_rows` carries its own codec, so the
            // encode would succeed and quietly produce bytes for another schema.
            throw new ArgumentException(
                "the batch is bound to a different codec, so encoding it here would produce " +
                "bytes for that codec's schema", nameof(rows));
        }

        rows.ThrowIfRowOutOfRange(row, nameof(row));

        using GrowingWindow window = new(output, RefillFaultForTest);
        FlError err = default;
        int status = window.Encode(rows.Handle, row, ref err);

        // The captured exception, if a refill threw, outranks the status the shim
        // synthesised from it (D-BIND-19 rule 3).
        Errors.ThrowIfFailed(status, ref err, window.Captured);
        window.Commit();
    }

    /// <summary>Decode one row.</summary>
    /// <param name="row">Exactly one encoded row, with nothing after it.</param>
    /// <returns>A one-row batch the caller owns.</returns>
    /// <exception cref="FletcherFormatException">The bytes are malformed.</exception>
    public RecordBatch Decode(ReadOnlySpan<byte> row) => DecodeBatch(row, count: 1);

    /// <summary>Decode <paramref name="count"/> rows written back to back.</summary>
    /// <param name="rows">The encoded rows, with no framing between them.</param>
    /// <param name="count">How many rows those bytes hold.</param>
    /// <returns>A batch the caller owns.</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="count"/> is negative.</exception>
    /// <exception cref="FletcherFormatException">
    /// The bytes are malformed, truncated, or hold a different number of rows.
    /// The message is the positional reader's own, verbatim.
    /// </exception>
    /// <remarks>
    /// The format is self-delimiting, so "where does row 2 begin" has exactly one
    /// answer; a <paramref name="count"/> that disagrees with the buffer is a
    /// refusal rather than a truncation, which is how a framing bug is caught
    /// instead of reaching an application as short data.
    ///
    /// Takes no lock the delivery path holds, so it is callable from inside a
    /// delivery callback — which is where a subscriber actually wants it.
    /// </remarks>
    public RecordBatch DecodeBatch(ReadOnlySpan<byte> rows, int count)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(count);
        ObjectDisposedException.ThrowIf(_disposed, this);

        CArrowArray* decoded = CArrowArray.Create();
        try
        {
            FlError err = default;
            int status;
            fixed (byte* bytes = rows)
            {
                status = NativeMethods.fl_decode_rows(
                    _codec, (nint)bytes, (nuint)rows.Length, count, (nint)decoded, ref err);
            }

            Errors.ThrowIfFailed(status, ref err);

            // The shim filled a fresh array the caller IMPORTS AND OWNS, and it is
            // imported against the DECODED schema, not the bound one: a dictionary
            // field went out as its value type and comes back as a plain value
            // array, so importing against `Schema` would ask Arrow to read a
            // dictionary's buffers from an array that has none. The two are the
            // same object whenever the schema carries no dictionary.
            return CArrowArrayImporter.ImportRecordBatch(decoded, DecodedSchema);
        }
        finally
        {
            // A failed decode leaves `decoded` untouched, and a successful one has
            // had its contents moved out by the import. Either way the structure
            // itself is this frame's to free.
            CArrowArray.Free(decoded);
        }
    }

    /// <summary>Close the codec.</summary>
    /// <remarks>
    /// A <see cref="BoundRows"/> still alive does not become invalid: the native
    /// <c>fl_rows</c> holds a share of the codec, so closing this first is
    /// survivable by construction rather than by the caller remembering an order.
    /// </remarks>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _codec.Dispose();
    }
}
