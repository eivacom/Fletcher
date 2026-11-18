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
// value by value, in tests/Shared/CoverageFixture.cs since BIND-8a, where the managed
// gateway codec is held to the same goldens through the same rows. A transcription slip
// would show as a byte difference, never as a pass, so a pass here means both the
// transcription and the two encoders agree.
//
// Each case also runs the other way: the C++ bytes decoded by the codec, read back through
// the generated FromArrow, re-encoded, and compared again - so C# reads what C++ writes.
using System;
using System.Buffers;
using System.Collections.Generic;
using System.IO;

using Apache.Arrow;

using Eiva.Fletcher.TestFixtures;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class CoverageWireBytesTests
{
    public static TheoryData<string> Goldens => CoverageFixture.Goldens;

    [Theory]
    [MemberData(nameof(Goldens))]
    public void CSharpWritesTheBytesCppWrites(string golden)
    {
        byte[] expected = ReadGolden(golden);
        (RecordBatch batch, _) = CoverageFixture.Case(golden);
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
        (RecordBatch fixture, Func<RecordBatch, RecordBatch> readBack) = CoverageFixture.Case(golden);
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

    private static string GoldenDir() => Path.Combine(AppContext.BaseDirectory, "coverage");

    private static byte[] ReadGolden(string name) => File.ReadAllBytes(Path.Combine(GoldenDir(), name));

    // Hex, so a failure shows WHERE the two encodings part rather than two byte arrays.
    private static string Hex(byte[] bytes) => Convert.ToHexString(bytes);
}
