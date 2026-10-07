// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6d-2: C#'s wire bytes against C++'s, for the same .proto and the same values.
//
// The C++ side is not built here. integration-tests/protoc-coverage already pins it: its
// parity oracle holds the C++ generated class's Encode() for the coverage fixture rows to
// committed goldens (golden/*.v1.bin), and those files are copied beside this assembly.
// The C# side is the plugin's own C# for the same coverage.proto, committed as
// golden/coverage.fletcher.cs (held to the plugin byte for byte by protoc-coverage's
// CsGolden.ModelByteIdentical) and compiled in here. So each case below is: the fixture
// row built in C#, through the generated ToArrow, bound and encoded by the native codec -
// the path every generated C# publisher takes - and compared with what C++ wrote.
//
// The fixture is a transcription of tests/coverage_fixture.hpp, function by function and
// value by value. A transcription slip would show as a byte difference, never as a pass,
// so a pass here means both the transcription and the two encoders agree.
//
// Each case also runs the other way: the C++ bytes decoded by the codec, read back through
// the generated FromArrow, re-encoded, and compared again - so C# reads what C++ writes.
using System;
using System.Buffers;
using System.Collections.Generic;
using System.IO;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher.Model;

using Fletcher.Gen.Integration.Coverage;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class CoverageWireBytesTests
{
    // ── The fixture, transcribed from integration-tests/protoc-coverage/tests/coverage_fixture.hpp ──
    private static readonly int KInt32 = -42;
    private static readonly long KInt64 = 9_000_000_000L;
    private static readonly int KOptInt32 = 7;
    private static readonly int KWrappedInt32 = 123;
    private static readonly long KTimestampNs = 1_700_000_000_000_000_000L;
    private static readonly long KDurationNs = 5_000_000_000L;

    private static ScalarCoverage MakeScalars() => new()
    {
        BoolValue = true,
        Int32Value = KInt32,
        Int64Value = KInt64,
        Uint32Value = 7u,
        Uint64Value = 8ul,
        Sint32Value = -9,
        Sint64Value = -10,
        Fixed32Value = 11u,
        Fixed64Value = 12ul,
        Sfixed32Value = -13,
        Sfixed64Value = -14,
        FloatValue = 1.5f,
        DoubleValue = 2.5,
        StringValue = "coverage",
        BytesValue = new byte[] { 0x01, 0x02, 0x03 },
        OptionalInt32 = KOptInt32,
        WrappedInt32 = KWrappedInt32,
        TimestampValue = new Timestamp(KTimestampNs, TimeUnit.Nanosecond),
        DurationValue = new Duration(KDurationNs, TimeUnit.Nanosecond),
        Status = TopLevelStatus.Warn,
        NestedStatus = NestedEnums_InnerStatus.Active,
    };

    private static ScalarCoverage MakeScalarsAllSet() => new()
    {
        BoolValue = false,
        Int32Value = 100,
        Int64Value = 200L,
        Uint32Value = 3u,
        Uint64Value = 4ul,
        Sint32Value = -5,
        Sint64Value = -6,
        Fixed32Value = 7u,
        Fixed64Value = 8ul,
        Sfixed32Value = -9,
        Sfixed64Value = -10,
        FloatValue = 0.5f,
        DoubleValue = 0.25,
        StringValue = "all-set",
        BytesValue = new byte[] { 0x0A, 0x0B, 0x0C, 0x0D },
        OptionalBool = true,
        OptionalString = "opt-str",
        OptionalBytes = new byte[] { 0x01, 0x02 },
        WrappedBool = true,
        WrappedInt64 = 123_456_789_012L,
        WrappedUint32 = 4_000_000_000u,
        WrappedUint64 = 9_000_000_000_000_000_000ul,
        WrappedFloat = 1.5f,
        WrappedDouble = 2.5,
        WrappedString = "wrapped-str",
        WrappedBytes = new byte[] { 0x03, 0x04, 0x05 },
        TimestampValue = new Timestamp(KTimestampNs, TimeUnit.Nanosecond),
        DurationValue = new Duration(KDurationNs, TimeUnit.Nanosecond),
        Status = TopLevelStatus.Error,
        NestedStatus = NestedEnums_InnerStatus.Disabled,
    };

    private static Leaf MakeLeaf(int id, string label, TopLevelStatus status) =>
        new() { Id = id, Label = label, Status = status };

    private static NestedEnums MakeNestedEnums() => new() { State = NestedEnums_InnerStatus.Disabled };

    private static Branch MakeBranch() => new()
    {
        Leaf = MakeLeaf(1, "root", TopLevelStatus.Ok),
        OptionalLeaf = MakeLeaf(2, "opt", TopLevelStatus.Warn),
        Leaves = { MakeLeaf(3, "a", TopLevelStatus.Ok), MakeLeaf(4, "b", TopLevelStatus.Warn) },
    };

    private static FlattenedPoint MakeFlattenedPoint() => new() { X = 1.25, Y = 2.5 };

    private static FieldFlattenedPosition MakeFieldFlattenedPosition() => new() { X = 3.75, Y = 4.5 };

    private static ServiceReply MakeServiceReply() => new() { Accepted = true, Message = "ok" };

    private static CompositeCoverage MakeComposite() => new()
    {
        Scalars = MakeScalars(),
        OptionalScalars = MakeScalars(),
        // optional_branch intentionally left unset.
        Branch = MakeBranch(),
        RepeatedScalar = { 10, 20, 30 },
        RepeatedString = { "x", "y" },
        // repeated_bytes: an empty container.
        RepeatedStruct = { MakeLeaf(5, "s0", TopLevelStatus.Ok), MakeLeaf(6, "s1", TopLevelStatus.Warn) },
        MapScalar = { new("a", 1), new("b", 2) },
        MapStruct = { new("k", MakeLeaf(7, "mk", TopLevelStatus.Warn)) },
        FlattenedStructList = { MakeLeaf(8, "f0", TopLevelStatus.Ok) },
        NestedStructLists = { new() { MakeLeaf(9, "n0", TopLevelStatus.Ok) }, new() },
        Depth3StructLists = { new() { new() { MakeLeaf(10, "d0", TopLevelStatus.Warn) } } },
        OptionalFlattenedStructList = { MakeLeaf(11, "of", TopLevelStatus.Ok) },
        MessageFlattenedPoint = MakeFlattenedPoint(),
        FieldFlattenedPosition = MakeFieldFlattenedPosition(),
    };

    private static CompositeCoverage MakeCompositeWithAlternateNullsAndEmpties() => new()
    {
        Scalars = MakeScalarsAllSet(),
        // optional_scalars intentionally unset.
        Branch = MakeBranch(),
        OptionalBranch = MakeBranch(),
        RepeatedBytes = { new byte[] { 0x09, 0x08 }, new byte[] { 0x07 } },
        OptionalFlattenedStructList = { MakeLeaf(30, "alt", TopLevelStatus.Error) },
        MessageFlattenedPoint = MakeFlattenedPoint(),
        FieldFlattenedPosition = MakeFieldFlattenedPosition(),
    };

    private static CompositeCoverage MakeCompositeWithMapsNonSorted()
    {
        CompositeCoverage c = MakeComposite();
        c.MapScalar = new() { new("z", 26), new("a", 1), new("m", 13) };
        c.MapStruct = new() { new("y", MakeLeaf(25, "y-leaf", TopLevelStatus.Warn)), new("b", MakeLeaf(2, "b-leaf", TopLevelStatus.Ok)) };
        return c;
    }

    private static ServiceRequest MakeServiceRequest() => new() { Payload = MakeComposite() };

    // ── One case per committed golden, as test_parity_oracle.cpp pairs them ──

    public static TheoryData<string> Goldens => new()
    {
        "coverage.ScalarCoverage.v1.bin",
        "coverage.ScalarCoverage.all-set.v1.bin",
        "coverage.CompositeCoverage.v1.bin",
        "coverage.CompositeCoverage.alternate-null-empty.v1.bin",
        "coverage.CompositeCoverage.maps-non-sorted.v1.bin",
        "coverage.Branch.v1.bin",
        "coverage.Leaf.v1.bin",
        "coverage.NestedEnums.v1.bin",
        "coverage.FlattenedPoint.v1.bin",
        "coverage.FieldFlattenedPosition.v1.bin",
        "coverage.ServiceRequest.v1.bin",
        "coverage.ServiceReply.v1.bin",
    };

    // The fixture row for a golden, as a one-row batch built by the generated ToArrow, and
    // the generated reader that turns a decoded batch back into a batch through FromArrow.
    private static (RecordBatch Batch, Func<RecordBatch, RecordBatch> ReadBack) Case(string golden) => golden switch
    {
        "coverage.ScalarCoverage.v1.bin" => (ScalarCoverage.ToArrow(new[] { MakeScalars() }), b => ScalarCoverage.ToArrow(new[] { ScalarCoverage.FromArrow(AsStruct(b), 0) })),
        "coverage.ScalarCoverage.all-set.v1.bin" => (ScalarCoverage.ToArrow(new[] { MakeScalarsAllSet() }), b => ScalarCoverage.ToArrow(new[] { ScalarCoverage.FromArrow(AsStruct(b), 0) })),
        "coverage.CompositeCoverage.v1.bin" => (CompositeCoverage.ToArrow(new[] { MakeComposite() }), b => CompositeCoverage.ToArrow(new[] { CompositeCoverage.FromArrow(AsStruct(b), 0) })),
        "coverage.CompositeCoverage.alternate-null-empty.v1.bin" => (CompositeCoverage.ToArrow(new[] { MakeCompositeWithAlternateNullsAndEmpties() }), b => CompositeCoverage.ToArrow(new[] { CompositeCoverage.FromArrow(AsStruct(b), 0) })),
        "coverage.CompositeCoverage.maps-non-sorted.v1.bin" => (CompositeCoverage.ToArrow(new[] { MakeCompositeWithMapsNonSorted() }), b => CompositeCoverage.ToArrow(new[] { CompositeCoverage.FromArrow(AsStruct(b), 0) })),
        "coverage.Branch.v1.bin" => (Branch.ToArrow(new[] { MakeBranch() }), b => Branch.ToArrow(new[] { Branch.FromArrow(AsStruct(b), 0) })),
        "coverage.Leaf.v1.bin" => (Leaf.ToArrow(new[] { MakeLeaf(42, "leaf", TopLevelStatus.Error) }), b => Leaf.ToArrow(new[] { Leaf.FromArrow(AsStruct(b), 0) })),
        "coverage.NestedEnums.v1.bin" => (NestedEnums.ToArrow(new[] { MakeNestedEnums() }), b => NestedEnums.ToArrow(new[] { NestedEnums.FromArrow(AsStruct(b), 0) })),
        "coverage.FlattenedPoint.v1.bin" => (FlattenedPoint.ToArrow(new[] { MakeFlattenedPoint() }), b => FlattenedPoint.ToArrow(new[] { FlattenedPoint.FromArrow(AsStruct(b), 0) })),
        "coverage.FieldFlattenedPosition.v1.bin" => (FieldFlattenedPosition.ToArrow(new[] { MakeFieldFlattenedPosition() }), b => FieldFlattenedPosition.ToArrow(new[] { FieldFlattenedPosition.FromArrow(AsStruct(b), 0) })),
        "coverage.ServiceRequest.v1.bin" => (ServiceRequest.ToArrow(new[] { MakeServiceRequest() }), b => ServiceRequest.ToArrow(new[] { ServiceRequest.FromArrow(AsStruct(b), 0) })),
        "coverage.ServiceReply.v1.bin" => (ServiceReply.ToArrow(new[] { MakeServiceReply() }), b => ServiceReply.ToArrow(new[] { ServiceReply.FromArrow(AsStruct(b), 0) })),
        _ => throw new ArgumentOutOfRangeException(nameof(golden), golden, "no fixture for this golden"),
    };

    [Theory]
    [MemberData(nameof(Goldens))]
    public void CSharpWritesTheBytesCppWrites(string golden)
    {
        byte[] expected = ReadGolden(golden);
        (RecordBatch batch, _) = Case(golden);
        using (batch)
        {
            Assert.Equal(Hex(expected), Hex(Encode(batch)));
        }
    }

    [Theory]
    [MemberData(nameof(Goldens))]
    public void CSharpReadsWhatCppWritesAndWritesItBackUnchanged(string golden)
    {
        byte[] expected = ReadGolden(golden);
        (RecordBatch fixture, Func<RecordBatch, RecordBatch> readBack) = Case(golden);
        using (fixture)
        {
            using var codec = new FletcherCodec(fixture.Schema);
            using RecordBatch decoded = codec.Decode(expected);
            using RecordBatch again = readBack(decoded);
            Assert.Equal(Hex(expected), Hex(Encode(again)));
        }
    }

    [Fact]
    public void EveryGoldenIsCoveredAndNoneIsMissing()
    {
        // A golden added to protoc-coverage without a case here would otherwise go
        // unchecked; one removed there would leave a case reading nothing.
        var onDisk = new SortedSet<string>(System.Array.ConvertAll(
            Directory.GetFiles(GoldenDir(), "coverage.*.v1.bin"), p => Path.GetFileName(p)!), StringComparer.Ordinal);
        var cases = new SortedSet<string>(StringComparer.Ordinal);
        foreach (string name in Goldens)
        {
            cases.Add(name);
        }

        Assert.Equal(onDisk, cases);
    }

    private static byte[] Encode(RecordBatch batch)
    {
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);
        var output = new ArrayBufferWriter<byte>();
        codec.Encode(rows, 0, output);
        return output.WrittenSpan.ToArray();
    }

    private static StructArray AsStruct(RecordBatch batch) =>
        new(new StructType(batch.Schema.FieldsList), batch.Length, batch.Arrays, ArrowBuffer.Empty, 0);

    private static string GoldenDir() => Path.Combine(AppContext.BaseDirectory, "coverage");

    private static byte[] ReadGolden(string name) => File.ReadAllBytes(Path.Combine(GoldenDir(), name));

    // Hex, so a failure shows WHERE the two encodings part rather than two byte arrays.
    private static string Hex(byte[] bytes) => Convert.ToHexString(bytes);
}
