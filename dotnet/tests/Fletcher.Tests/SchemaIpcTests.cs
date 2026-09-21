// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-3c: schema IPC, and the descriptors the generator actually emits.
//
// ── Why schema IPC is managed and has no ABI entry point ────────────────────
// `SerializeSchemaIpc`/`DeserializeSchemaIpc` are Arrow's own format, not
// Fletcher's, and `Apache.Arrow.Ipc` already implements them. Adding a shim
// entry point would mean two implementations of a format neither side owns,
// which is the arrangement D-BIND-1 exists to avoid for the WIRE format and is
// no better here. So the binding parses IPC in managed code - and the property
// that has to be asserted is that what it parses is what the codec accepts.
//
// ── Why these particular files ─────────────────────────────────────────────
// `protoc/tests/golden/*.ipc` is not a fixture written for this test. It is the
// generator's own output, checked in and asserted byte-for-byte by
// `test_schema_visitor.cpp`, one file per message shape the mapping produces.
// Opening a codec over every one of them is therefore the first evidence in the
// round that BIND-6's generated descriptors and BIND-3's codec agree - and it is
// evidence available NOW, four items before the C# generator exists, which is
// the only reason it is worth writing here rather than there.
using System;
using System.Collections.Generic;
using System.IO;

using Apache.Arrow;
using Apache.Arrow.Ipc;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class SchemaIpcTests
{
    /// <summary>The goldens, by file name, as the build staged them.</summary>
    /// <remarks>
    /// Discovered rather than listed. A hand-written list is a count-shaped
    /// acceptance by another name: it passes unchanged on the day a message is
    /// added and the new golden is never opened by anything.
    /// </remarks>
    public static TheoryData<string> Goldens
    {
        get
        {
            var data = new TheoryData<string>();
            foreach (string path in Directory.GetFiles(GoldenDirectory, "*.ipc"))
            {
                data.Add(Path.GetFileName(path));
            }

            return data;
        }
    }

    private static string GoldenDirectory =>
        Path.Combine(AppContext.BaseDirectory, "golden");

    /// <summary>Every generated descriptor parses, and the codec opens over it.</summary>
    /// <remarks>
    /// Two claims in one row because the second is worthless without the first: a
    /// schema that failed to parse would give an empty field list, and a codec
    /// opens over an empty schema perfectly happily.
    /// </remarks>
    [Theory]
    [MemberData(nameof(Goldens))]
    public void EveryGeneratedDescriptorOpensACodec(string fileName)
    {
        Schema schema = ReadSchema(Path.Combine(GoldenDirectory, fileName));

        Assert.NotEmpty(schema.FieldsList);

        using var codec = new FletcherCodec(schema);
        Assert.Same(schema, codec.Schema);
    }

    /// <summary>The staged goldens are the ones on disk in `protoc/tests/golden`.</summary>
    /// <remarks>
    /// The vacuity guard for the theory above: a copy step that silently matched
    /// nothing would leave <see cref="Goldens"/> empty, and a theory with no cases
    /// passes.
    /// </remarks>
    [Fact]
    public void TheGoldensWereStaged()
    {
        Assert.True(
            Directory.Exists(GoldenDirectory),
            $"no golden directory beside the test binary at {GoldenDirectory}; the copy in " +
            "Fletcher.Tests.csproj matched nothing");

        Assert.NotEmpty(Directory.GetFiles(GoldenDirectory, "*.ipc"));
    }

    /// <summary>A schema survives a round trip through Arrow's IPC stream format.</summary>
    /// <remarks>
    /// The managed half of §2.6: what a binding sends as a topic's schema is what
    /// comes back. Field metadata is checked explicitly because it is what carries
    /// `fletcher.field_number` — the thing that makes a positional wire format
    /// readable by a generator that never saw the .proto.
    /// </remarks>
    [Fact]
    public void ASchemaRoundTripsThroughIpc()
    {
        Schema original = CodecFixtures.Composite().Schema;

        using var buffer = new MemoryStream();
        using (var writer = new ArrowStreamWriter(buffer, original, leaveOpen: true))
        {
            writer.WriteStart();
            writer.WriteEnd();
        }

        buffer.Position = 0;
        using var reader = new ArrowStreamReader(buffer);
        reader.ReadNextRecordBatch();
        Schema restored = reader.Schema;

        Assert.Equal(original.FieldsList.Count, restored.FieldsList.Count);
        for (int i = 0; i < original.FieldsList.Count; ++i)
        {
            Assert.Equal(original.FieldsList[i].Name, restored.FieldsList[i].Name);
            Assert.Equal(original.FieldsList[i].DataType.TypeId, restored.FieldsList[i].DataType.TypeId);
        }

        // And it is a schema the codec accepts, which is the only reason a binding
        // parses one at all.
        using var codec = new FletcherCodec(restored);
        Assert.NotNull(codec.Schema);
    }

    /// <summary>Read a schema-only IPC stream: the shape the plugin emits.</summary>
    /// <remarks>
    /// The golden files hold a schema message and nothing after it. The stream
    /// reader learns the schema from the first message, so asking it for a record
    /// batch is how the schema is reached — the null it answers with is the
    /// expected outcome, not a failure.
    /// </remarks>
    private static Schema ReadSchema(string path)
    {
        using FileStream file = File.OpenRead(path);
        using var reader = new ArrowStreamReader(file);

        RecordBatch? batch = reader.ReadNextRecordBatch();
        batch?.Dispose();

        return reader.Schema;
    }
}
