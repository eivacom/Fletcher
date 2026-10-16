// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8a: the managed gateway codec's wire bytes against C++'s.
//
// The byte-parity evidence for the one managed codec (D-BIND-1), and the same evidence
// Fletcher.Tests gives for the native one: the protoc-coverage fixture rows
// (tests/Shared/CoverageFixture.cs, a transcription of that lane's coverage_fixture.hpp)
// built by the generated ToArrow, encoded here, and compared with the C++ generated
// class's Encode() of the same rows, committed as protoc-coverage's golden/*.v1.bin and
// pinned there by its parity oracle. Then the other way: the C++ bytes decoded here, read
// back through the generated FromArrow, encoded again, compared again.
//
// The two codecs are thereby held to ONE set of C++ bytes through ONE set of rows, so
// they agree with each other wherever these rows reach: every shape the proto mapping
// produces (scalars of every proto type, optional and wrapped values, enums, Timestamp
// and Duration, nested and optional messages, repeated scalars, strings, bytes and
// messages, both map kinds, the flatten forms, lists of lists to depth 3) and the
// null / empty / defaulted variants of each.
using System;
using System.Collections.Generic;
using System.IO;

using Apache.Arrow;

using Eiva.Fletcher.TestFixtures;

using Xunit;

namespace Eiva.Fletcher.GatewayClient.Tests;

public sealed class CoverageWireBytesTests
{
    public static TheoryData<string> Goldens => CoverageFixture.Goldens;

    [Theory]
    [MemberData(nameof(Goldens))]
    public void TheManagedCodecWritesTheBytesCppWrites(string golden)
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
    public void TheManagedCodecReadsWhatCppWritesAndWritesItBackUnchanged(string golden)
    {
        byte[] expected = ReadGolden(golden);
        (RecordBatch fixture, Func<RecordBatch, RecordBatch> readBack) = CoverageFixture.Case(golden);
        using (fixture)
        {
            var codec = new GatewayRowCodec(fixture.Schema);
            using RecordBatch decoded = codec.Decode(expected);
            Assert.Equal(1, decoded.Length);
            using RecordBatch again = readBack(decoded);
            Assert.Equal(Hex(expected), Hex(Encode(again)));
        }
    }

    [Fact]
    public void EveryGoldenIsCoveredAndNoneIsMissing()
    {
        var onDisk = new SortedSet<string>(System.Array.ConvertAll(
            Directory.GetFiles(GoldenDir(), "coverage.*.v1.bin"), p => Path.GetFileName(p)!), StringComparer.Ordinal);
        var cases = new SortedSet<string>(StringComparer.Ordinal);
        foreach (string name in Goldens)
        {
            cases.Add(name);
        }

        Assert.Equal(onDisk, cases);
    }

    [Fact]
    public void SeveralRowsDecodeIntoOneBatchThatReadsBackRowByRow()
    {
        // The subscriber path decodes row by row as MESSAGE frames arrive; DecodeBatch is
        // the same walk over several. Three different C++-written rows of one schema.
        string[] names =
        {
            "coverage.CompositeCoverage.v1.bin",
            "coverage.CompositeCoverage.alternate-null-empty.v1.bin",
            "coverage.CompositeCoverage.defaulted.v1.bin",
        };
        (RecordBatch fixture, _) = CoverageFixture.Case(names[0]);
        using (fixture)
        {
            var codec = new GatewayRowCodec(fixture.Schema);
            var rows = new List<ReadOnlyMemory<byte>>();
            foreach (string name in names)
            {
                rows.Add(ReadGolden(name));
            }

            using RecordBatch decoded = codec.DecodeBatch(rows);
            Assert.Equal(names.Length, decoded.Length);
            BoundBatch bound = codec.Bind(decoded);
            for (int i = 0; i < names.Length; ++i)
            {
                Assert.Equal(Hex(ReadGolden(names[i])), Hex(codec.Encode(bound, i)));
            }
        }
    }

    private static byte[] Encode(RecordBatch batch)
    {
        var codec = new GatewayRowCodec(batch.Schema);
        return codec.Encode(codec.Bind(batch), 0);
    }

    private static string GoldenDir() => Path.Combine(AppContext.BaseDirectory, "coverage");

    private static byte[] ReadGolden(string name) => File.ReadAllBytes(Path.Combine(GoldenDir(), name));

    // Hex, so a failure shows WHERE the two encodings part rather than two byte arrays.
    private static string Hex(byte[] bytes) => Convert.ToHexString(bytes);
}
