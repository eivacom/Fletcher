// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-3c: the codec tier, against the real shim.
//
// The property that carries the most weight here is the ROUND TRIP IN BOTH
// DIRECTIONS - decode the bytes, then re-encode the decoded batch and compare
// the bytes, AND compare the values. Either alone has a blind spot: value
// equality misses an encoder and decoder that agree on a wrong layout, and byte
// equality misses a pair that lose a value identically in both directions. The
// C++ suite makes the same argument and holds itself to the same pair.
//
// What is deliberately NOT asserted here: that the bytes are the ones
// `arrow-bridge` produces. That is byte identity against an independent
// implementation, it is asserted in `test_nanoarrow_codec.cpp` where a second
// encoder exists, and repeating it here would need a managed encoder - which
// D-BIND-1 says must never exist.
using System;
using System.Buffers;
using System.Collections.Generic;

using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class CodecTests
{
    /// <summary>A schema the wire format carries opens, and remembers itself.</summary>
    [Fact]
    public void ACodecOpensOverAMappedSchema()
    {
        RecordBatch batch = CodecFixtures.Composite();

        using var codec = new FletcherCodec(batch.Schema);

        Assert.Same(batch.Schema, codec.Schema);
    }

    /// <summary>Binding validates the batch once and reports its length.</summary>
    [Fact]
    public void BindingABatchYieldsItsRows()
    {
        RecordBatch batch = CodecFixtures.Composite();
        using var codec = new FletcherCodec(batch.Schema);

        using BoundRows rows = codec.Bind(batch);

        Assert.Equal(batch.Length, rows.Length);
        Assert.Same(codec.Schema, rows.Schema);
    }

    /// <summary>
    /// The borrow rule: binding does not consume the caller's export, and neither
    /// does encoding every row of it.
    /// </summary>
    /// <remarks>
    /// This is the managed twin of the C++ suite's
    /// <c>"fl_rows_bind consumed the caller's array"</c> assertion, and it is the
    /// property ONE EXPORT SERVING N PUBLISHES rests on. If the shim ever called
    /// the release callback, the first encode after it would read freed buffers —
    /// and would usually still succeed, which is why this is asserted directly
    /// rather than inferred from a passing round trip.
    /// </remarks>
    [Fact]
    public void BindingBorrowsTheExportAndNeverConsumesIt()
    {
        RecordBatch batch = CodecFixtures.Composite();
        using var codec = new FletcherCodec(batch.Schema);

        using BoundRows rows = codec.Bind(batch);
        Assert.True(rows.Export.IsIntact, "fl_rows_bind consumed the caller's array");

        var output = new ArrayBufferWriter<byte>();
        for (int row = 0; row < rows.Length; ++row)
        {
            codec.Encode(rows, row, output);
            Assert.True(rows.Export.IsIntact, $"encoding row {row} consumed the caller's array");
        }
    }

    /// <summary>One bind serves N encodes, and every row lands.</summary>
    [Fact]
    public void EveryRowEncodesToSomething()
    {
        RecordBatch batch = CodecFixtures.Composite();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        var sizes = new List<int>();
        for (int row = 0; row < rows.Length; ++row)
        {
            var output = new ArrayBufferWriter<byte>();
            codec.Encode(rows, row, output);
            sizes.Add(output.WrittenCount);
        }

        Assert.Equal(batch.Length, sizes.Count);
        Assert.All(sizes, size => Assert.True(size > 0, "a row encoded to nothing"));

        // Vacuity guard with teeth: the rows of this fixture differ in shape, so
        // identical sizes would mean the encoder is writing something that does
        // not depend on the row.
        Assert.True(
            new HashSet<int>(sizes).Count > 1,
            "every row encoded to the same number of bytes, so the encoder may not be reading them");
    }

    /// <summary>Decode is the inverse of encode, proven in both directions.</summary>
    [Fact]
    public void DecodeIsTheInverseOfEncode()
    {
        RecordBatch batch = CodecFixtures.Composite();
        using var codec = new FletcherCodec(batch.Schema);

        byte[] encoded;
        using (BoundRows rows = codec.Bind(batch))
        {
            encoded = CodecFixtures.EncodeAll(codec, rows);
        }

        Assert.NotEmpty(encoded);

        using RecordBatch decoded = codec.DecodeBatch(encoded, batch.Length);
        Assert.Equal(batch.Length, decoded.Length);
        Assert.Equal(batch.ColumnCount, decoded.ColumnCount);

        // Direction one: the decoded batch re-encodes to the same bytes.
        using (BoundRows again = codec.Bind(decoded))
        {
            Assert.Equal(encoded, CodecFixtures.EncodeAll(codec, again));
        }

        // Direction two: the values survived. Re-encoding identically would also
        // hold for an encoder and decoder that lost the same value in both
        // directions, which is exactly what this catches.
        AssertSameValues(batch, decoded);
    }

    /// <summary>One row at a time, through the single-row entry point.</summary>
    [Fact]
    public void ASingleRowDecodesOnItsOwn()
    {
        RecordBatch batch = CodecFixtures.Composite();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        for (int row = 0; row < rows.Length; ++row)
        {
            var output = new ArrayBufferWriter<byte>();
            codec.Encode(rows, row, output);

            using RecordBatch decoded = codec.Decode(output.WrittenSpan);

            Assert.Equal(1, decoded.Length);
            Assert.Equal(batch.ColumnCount, decoded.ColumnCount);
        }
    }

    /// <summary>A dictionary column is refused at open, and the refusal names it.</summary>
    /// <remarks>
    /// D-BIND-8: a dictionary is a columnar optimisation and the wire format
    /// carries the VALUE type one value per row, so a dictionary column is
    /// something the tier above lowers — never something this codec invents a
    /// representation for while the DICT round is halted.
    ///
    /// The message naming the field is the point. A schema has many columns and
    /// "dictionary fields are not supported" leaves the caller to find which.
    /// </remarks>
    [Fact]
    public void ADictionaryColumnIsRefusedByName()
    {
        var schema = new Schema(
        [
            new Field("id", Int32Type.Default, nullable: true),
            new Field("category", new DictionaryType(Int32Type.Default, StringType.Default, ordered: false), nullable: true),
        ], metadata: null);

        FletcherFormatException refusal =
            Assert.Throws<FletcherFormatException>(() => new FletcherCodec(schema));

        Assert.Equal(FletcherStatus.InvalidArgument, refusal.Status);
        Assert.Contains("category", refusal.Message, StringComparison.Ordinal);
        Assert.Contains("dictionary", refusal.Message, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>A batch that is not this codec's schema is refused at bind.</summary>
    [Fact]
    public void ABatchOfTheWrongSchemaIsRefusedAtBind()
    {
        using var codec = new FletcherCodec(CodecFixtures.Composite().Schema);

        Assert.Throws<FletcherFormatException>(() => codec.Bind(CodecFixtures.Scalar()));
    }

    /// <summary>
    /// A batch bound to ANOTHER codec is refused here, by this tier rather than by
    /// native.
    /// </summary>
    /// <remarks>
    /// Native cannot catch it: an `fl_rows` carries its own codec, so the encode
    /// would succeed and produce bytes for the other schema. Silent wrong output
    /// is the worst failure available at this boundary, so the check is a managed
    /// one and it is an <see cref="ArgumentException"/>, not a Fletcher failure.
    /// </remarks>
    [Fact]
    public void EncodingRowsBoundToAnotherCodecIsRefused()
    {
        RecordBatch batch = CodecFixtures.Composite();
        using var mine = new FletcherCodec(batch.Schema);
        using var theirs = new FletcherCodec(batch.Schema);
        using BoundRows rows = theirs.Bind(batch);

        ArgumentException refusal = Assert.Throws<ArgumentException>(
            () => mine.Encode(rows, 0, new ArrayBufferWriter<byte>()));

        Assert.Equal("rows", refusal.ParamName);
    }

    /// <summary>A row index outside the batch says which argument was wrong.</summary>
    [Fact]
    public void ARowOutsideTheBatchIsAnArgumentOutOfRange()
    {
        RecordBatch batch = CodecFixtures.Composite();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        Assert.Throws<ArgumentOutOfRangeException>(
            () => codec.Encode(rows, rows.Length, new ArrayBufferWriter<byte>()));
        Assert.Throws<ArgumentOutOfRangeException>(
            () => codec.Encode(rows, -1, new ArrayBufferWriter<byte>()));
    }

    /// <summary>
    /// Closing the codec before unbinding its rows is survivable, because the
    /// native view holds a share of it.
    /// </summary>
    /// <remarks>
    /// `Handles.cs` declines to add a managed AddRef here on the grounds that the
    /// shim already removed the hazard. That is a claim about someone else's code,
    /// so it is asserted rather than trusted: this disposes in the order the
    /// header says is safe and then uses the rows.
    /// </remarks>
    [Fact]
    public void ClosingTheCodecBeforeUnbindingIsSurvivable()
    {
        RecordBatch batch = CodecFixtures.Composite();
        var codec = new FletcherCodec(batch.Schema);
        BoundRows rows = codec.Bind(batch);

        codec.Dispose();

        // The rows are still unbindable-safe: this is the order that would be a
        // use-after-free if `fl_rows` held a raw pointer to the codec.
        rows.Dispose();
    }

    /// <summary>A disposed codec refuses rather than crossing with a dead handle.</summary>
    [Fact]
    public void ADisposedCodecRefusesEverything()
    {
        RecordBatch batch = CodecFixtures.Composite();
        var codec = new FletcherCodec(batch.Schema);
        codec.Dispose();

        Assert.Throws<ObjectDisposedException>(() => codec.Bind(batch));
        Assert.Throws<ObjectDisposedException>(() => codec.DecodeBatch(new byte[] { 0 }, 1));

        // Disposing twice is a no-op, which is what makes `using` safe to pair
        // with an explicit close.
        codec.Dispose();
    }

    // ── comparisons ─────────────────────────────────────────────────────────

    /// <summary>Column for column, value for value, through the Arrow visitors.</summary>
    private static void AssertSameValues(RecordBatch expected, RecordBatch actual)
    {
        Assert.Equal(expected.ColumnCount, actual.ColumnCount);

        for (int column = 0; column < expected.ColumnCount; ++column)
        {
            IArrowArray left = expected.Column(column);
            IArrowArray right = actual.Column(column);

            Assert.Equal(left.Length, right.Length);
            Assert.Equal(left.NullCount, right.NullCount);

            for (int row = 0; row < left.Length; ++row)
            {
                Assert.Equal(left.IsNull(row), right.IsNull(row));
            }
        }

        // The scalar columns are compared by value; the composite ones are
        // compared by their own children's values, recursively.
        AssertInt32Column(expected, actual, 0);
        AssertStringColumn(expected, actual, 1);
        AssertListOfInt32Column(expected, actual, 2);
        AssertStructColumn(expected, actual, 3);
        AssertMapColumn(expected, actual, 4);
    }

    private static void AssertInt32Column(RecordBatch expected, RecordBatch actual, int column)
    {
        var left = (Int32Array)expected.Column(column);
        var right = (Int32Array)actual.Column(column);
        for (int row = 0; row < left.Length; ++row)
        {
            Assert.Equal(left.GetValue(row), right.GetValue(row));
        }
    }

    private static void AssertStringColumn(RecordBatch expected, RecordBatch actual, int column)
    {
        var left = (StringArray)expected.Column(column);
        var right = (StringArray)actual.Column(column);
        for (int row = 0; row < left.Length; ++row)
        {
            Assert.Equal(left.GetString(row), right.GetString(row));
        }
    }

    private static void AssertListOfInt32Column(RecordBatch expected, RecordBatch actual, int column)
    {
        var left = (ListArray)expected.Column(column);
        var right = (ListArray)actual.Column(column);
        for (int row = 0; row < left.Length; ++row)
        {
            if (left.IsNull(row))
            {
                continue;
            }

            var leftValues = (Int32Array)left.GetSlicedValues(row);
            var rightValues = (Int32Array)right.GetSlicedValues(row);
            Assert.Equal(leftValues.Length, rightValues.Length);
            for (int i = 0; i < leftValues.Length; ++i)
            {
                Assert.Equal(leftValues.GetValue(i), rightValues.GetValue(i));
            }
        }
    }

    private static void AssertStructColumn(RecordBatch expected, RecordBatch actual, int column)
    {
        var left = (StructArray)expected.Column(column);
        var right = (StructArray)actual.Column(column);

        var leftIds = (Int32Array)left.Fields[0];
        var rightIds = (Int32Array)right.Fields[0];
        var leftLabels = (StringArray)left.Fields[1];
        var rightLabels = (StringArray)right.Fields[1];

        for (int row = 0; row < left.Length; ++row)
        {
            if (left.IsNull(row))
            {
                continue;
            }

            Assert.Equal(leftIds.GetValue(row), rightIds.GetValue(row));
            Assert.Equal(leftLabels.GetString(row), rightLabels.GetString(row));
        }
    }

    private static void AssertMapColumn(RecordBatch expected, RecordBatch actual, int column)
    {
        var left = (MapArray)expected.Column(column);
        var right = (MapArray)actual.Column(column);

        for (int row = 0; row < left.Length; ++row)
        {
            if (left.IsNull(row))
            {
                continue;
            }

            // A map's values are entry structs: {key, value}. Both halves are
            // compared, because a map that kept its keys and lost its values
            // would otherwise read as a pass.
            var leftEntries = (StructArray)left.GetSlicedValues(row);
            var rightEntries = (StructArray)right.GetSlicedValues(row);
            Assert.Equal(leftEntries.Length, rightEntries.Length);

            var leftKeys = (StringArray)leftEntries.Fields[0];
            var rightKeys = (StringArray)rightEntries.Fields[0];
            var leftValues = (Int32Array)leftEntries.Fields[1];
            var rightValues = (Int32Array)rightEntries.Fields[1];

            for (int i = 0; i < leftEntries.Length; ++i)
            {
                Assert.Equal(leftKeys.GetString(i), rightKeys.GetString(i));
                Assert.Equal(leftValues.GetValue(i), rightValues.GetValue(i));
            }
        }
    }
}
