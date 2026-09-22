// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The batches the codec tier is tested against.
//
// One shape, deliberately: a schema that carries every structural feature the
// wire format back-patches - a null bitfield, a length-prefixed string, a list,
// a nested struct and a map - because those are the parts an encode writes
// BELOW its own cursor and therefore the parts a refill can lose. A fixture of
// four scalars would encode into a handful of bytes, fit any window, and prove
// nothing about the two things this slice actually adds: growth and framing.
using System;

using Apache.Arrow;
using Apache.Arrow.Types;

namespace Eiva.Fletcher.Tests;

/// <summary>Batches shared by the codec, window and malformed-input tests.</summary>
internal static class CodecFixtures
{
    /// <summary>The struct type used as a nested column and as a list's item.</summary>
    internal static StructType Inner { get; } = new StructType(
    [
        new Field("id", Int32Type.Default, nullable: true),
        new Field("label", StringType.Default, nullable: true),
    ]);

    /// <summary>Three rows over scalars, a string, a list, a struct and a map.</summary>
    /// <remarks>
    /// Row 3 is null wherever a column can be null, so every validity bitmap has
    /// something to carry. A fixture whose bitmaps are all ones would not notice a
    /// bitfield that failed to cross — or one that a refill dropped.
    /// </remarks>
    internal static RecordBatch Composite()
    {
        Int32Array ids = new Int32Array.Builder().Append(1).Append(-2).AppendNull().Build();

        // Non-ASCII on purpose: the boundary is UTF-16 in managed memory and UTF-8
        // on the wire, and an ASCII-only fixture would not notice a wrong length.
        StringArray names = new StringArray.Builder()
            .Append("fletcher").Append("blåbærgrød").AppendNull().Build();

        var scoreBuilder = new ListArray.Builder(Int32Type.Default);
        var scores = (Int32Array.Builder)scoreBuilder.ValueBuilder;
        scoreBuilder.Append();
        scores.Append(10).Append(20).Append(30);
        scoreBuilder.Append();
        scores.Append(-1);
        scoreBuilder.AppendNull();

        var tagBuilder = new MapArray.Builder(
            new MapType(StringType.Default, Int32Type.Default, keySorted: false));
        var keys = (StringArray.Builder)tagBuilder.KeyBuilder;
        var values = (Int32Array.Builder)tagBuilder.ValueBuilder;
        tagBuilder.Append();
        keys.Append("alpha");
        values.Append(1);
        keys.Append("beta");
        values.Append(2);
        tagBuilder.Append();
        keys.Append("gamma");
        values.Append(3);
        tagBuilder.AppendNull();

        var schema = new Schema(
        [
            new Field("id", Int32Type.Default, nullable: true),
            new Field("name", StringType.Default, nullable: true),
            new Field("scores", new ListType(new Field("item", Int32Type.Default, nullable: true)), nullable: true),
            new Field("inner", Inner, nullable: true),
            new Field("tags", new MapType(StringType.Default, Int32Type.Default, keySorted: false), nullable: true),
        ], metadata: null);

        return new RecordBatch(
            schema,
            [ids, names, scoreBuilder.Build(), BuildInner(), tagBuilder.Build()],
            length: 3);
    }

    /// <summary>One row that cannot fit a first window, for the refill path.</summary>
    /// <param name="stringLength">How long the row's one string is, in bytes.</param>
    /// <remarks>
    /// The refill path is only reachable with a row bigger than the window the
    /// encode starts with, and the encode starts with whatever the caller's writer
    /// hands back for a 512-byte hint. So this fixture does not ask a writer to
    /// misbehave — it produces a genuinely large row, which is the case a refill
    /// exists for, and asserting that the row exceeds that hint is what makes the
    /// refill certain rather than hoped for.
    /// </remarks>
    internal static RecordBatch Large(int stringLength)
    {
        var schema = new Schema(
        [
            new Field("id", Int32Type.Default, nullable: true),
            new Field("blob", StringType.Default, nullable: true),
        ], metadata: null);

        // Not a repeated character: a run of one byte would survive a refill that
        // shifted the contents, and the point of the comparison this feeds is to
        // catch exactly that.
        var text = new char[stringLength];
        for (int i = 0; i < text.Length; ++i)
        {
            text[i] = (char)('a' + (i % 26));
        }

        return new RecordBatch(
            schema,
            [
                new Int32Array.Builder().Append(1).Build(),
                new StringArray.Builder().Append(new string(text)).Build(),
            ],
            length: 1);
    }

    /// <summary>Three rows with a dictionary-encoded string column (D-BIND-39).</summary>
    /// <remarks>
    /// The indices are deliberately NOT in order and do not start at zero. An
    /// encoder that put the index on the wire, or that resolved through the wrong
    /// slot, would then produce different bytes from
    /// <see cref="DictionaryAsPlainValues"/> rather than accidentally the same
    /// ones — which is the whole point of that pairing.
    /// </remarks>
    internal static RecordBatch Dictionary()
    {
        var schema = new Schema(
        [
            new Field("id", Int32Type.Default, nullable: true),
            new Field(
                "category",
                new DictionaryType(Int32Type.Default, StringType.Default, ordered: false),
                nullable: true),
        ], metadata: null);

        StringArray values = new StringArray.Builder().Append("alpha").Append("beta").Append("gamma").Build();
        Int32Array indices = new Int32Array.Builder().Append(2).Append(0).Append(1).Build();

        var dictionary = new DictionaryArray(
            (DictionaryType)schema.FieldsList[1].DataType, indices, values);

        return new RecordBatch(
            schema,
            [new Int32Array.Builder().Append(1).Append(2).Append(3).Build(), dictionary],
            length: 3);
    }

    /// <summary>A dictionary nested inside a struct, and one inside a list.</summary>
    /// <remarks>
    /// D1's subject. A TOP-LEVEL dictionary resolves through one call and was
    /// already covered; a nested one makes the rewrite RECURSE, and the failure it
    /// guards against is not a refusal but a disagreement - `DecodeRows` building
    /// an array against one reading of the schema while the managed importer reads
    /// another. That either throws at the caller or misreads buffers, and the
    /// second is worse because it produces values.
    /// </remarks>
    internal static RecordBatch NestedDictionary()
    {
        var dictionaryType = new DictionaryType(Int32Type.Default, StringType.Default, ordered: false);
        var innerStruct = new StructType([new Field("category", dictionaryType, nullable: true)]);

        var schema = new Schema(
        [
            new Field("id", Int32Type.Default, nullable: true),
            new Field("tagged", innerStruct, nullable: true),
        ], metadata: null);

        StringArray values = new StringArray.Builder().Append("alpha").Append("beta").Append("gamma").Build();
        Int32Array indices = new Int32Array.Builder().Append(2).Append(0).Append(1).Build();
        var nested = new DictionaryArray(dictionaryType, indices, values);

        // Every row valid: the subject here is the nested dictionary, not null
        // handling, and a null parent would suppress the child values on the wire
        // (a null struct's children are not transmitted, by design) - which would
        // make the assertions below test the wrong thing.
        var validity = new ArrowBuffer.BitmapBuilder(3);
        validity.Append(true);
        validity.Append(true);
        validity.Append(true);

        var tagged = new StructArray(innerStruct, length: 3, [nested], validity.Build(), nullCount: 0);

        return new RecordBatch(
            schema,
            [new Int32Array.Builder().Append(1).Append(2).Append(3).Build(), tagged],
            length: 3);
    }

    /// <summary>The same values as <see cref="Dictionary"/>, as a plain utf8 column.</summary>
    /// <remarks>
    /// The independent subject the dictionary's bytes are compared against. Not
    /// derived from the batch above — written out in the resolved order by hand —
    /// because a fixture computed from the thing it checks proves nothing.
    /// </remarks>
    internal static RecordBatch DictionaryAsPlainValues()
    {
        var schema = new Schema(
        [
            new Field("id", Int32Type.Default, nullable: true),
            new Field("category", StringType.Default, nullable: true),
        ], metadata: null);

        return new RecordBatch(
            schema,
            [
                new Int32Array.Builder().Append(1).Append(2).Append(3).Build(),
                new StringArray.Builder().Append("gamma").Append("alpha").Append("beta").Build(),
            ],
            length: 3);
    }

    /// <summary>A batch of one nullable int32 column, for cases that need no shape.</summary>
    internal static RecordBatch Scalar()
    {
        var schema = new Schema([new Field("id", Int32Type.Default, nullable: true)], metadata: null);
        Int32Array ids = new Int32Array.Builder().Append(7).Append(8).Build();
        return new RecordBatch(schema, [ids], length: 2);
    }

    /// <summary>Every row of <paramref name="rows"/>, encoded back to back.</summary>
    /// <remarks>
    /// The shape <c>DecodeBatch</c> is handed: N rows, no framing between them.
    /// The format is self-delimiting, so where row 2 begins has exactly one answer.
    /// </remarks>
    internal static byte[] EncodeAll(FletcherCodec codec, BoundRows rows)
    {
        var output = new System.Buffers.ArrayBufferWriter<byte>();
        for (int row = 0; row < rows.Length; ++row)
        {
            codec.Encode(rows, row, output);
        }

        return output.WrittenSpan.ToArray();
    }

    private static StructArray BuildInner()
    {
        Int32Array ids = new Int32Array.Builder().Append(11).Append(12).AppendNull().Build();
        StringArray labels = new StringArray.Builder().Append("a").AppendNull().Append("c").Build();

        var validity = new ArrowBuffer.BitmapBuilder(3);
        validity.Append(true);
        validity.Append(true);
        validity.Append(false);

        return new StructArray(Inner, length: 3, [ids, labels], validity.Build(), nullCount: 1);
    }
}
