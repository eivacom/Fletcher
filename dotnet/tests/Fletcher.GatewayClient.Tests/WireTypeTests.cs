// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8a: the wire-type table, held to the two it must equal.
//
// The numbers in a JSON schema descriptor's `wireType` are the gateway's: its
// NanoarrowTypeToWireType writes them, its create_topic parser reads them, and the
// TypeScript client keeps its own copy as an enum. A third hand-kept copy in C# would
// drift silently, so these tests read the other two FROM SOURCE (the build copies
// gateway/src/schema_codec.cpp and gateway-client-ts/src/wire-types.ts beside this
// assembly) and compare:
//
// * C# enum == TypeScript enum, name for name and number for number;
// * WireTypes.Of(arrow type) == the gateway's number for that type, for every arm of the
//   gateway's switch. This is test_schema_codec.cpp's WireTypeRoundTripTest seen from
//   the client: there the gateway's two switches are held to each other, here the
//   client's table is held to the gateway's.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.GatewayClient.Tests;

public sealed class WireTypeTests
{
    // An Arrow type for every nanoarrow type the gateway's switch names.
    private static readonly Dictionary<string, IArrowType> ArrowFor = new(StringComparer.Ordinal)
    {
        ["BOOL"] = BooleanType.Default,
        ["INT8"] = Int8Type.Default,
        ["INT16"] = Int16Type.Default,
        ["INT32"] = Int32Type.Default,
        ["INT64"] = Int64Type.Default,
        ["UINT8"] = UInt8Type.Default,
        ["UINT16"] = UInt16Type.Default,
        ["UINT32"] = UInt32Type.Default,
        ["UINT64"] = UInt64Type.Default,
        ["FLOAT"] = FloatType.Default,
        ["DOUBLE"] = DoubleType.Default,
        ["STRING"] = StringType.Default,
        ["BINARY"] = BinaryType.Default,
        ["DATE32"] = Date32Type.Default,
        ["DATE64"] = Date64Type.Default,
        ["TIMESTAMP"] = new TimestampType(TimeUnit.Nanosecond, (string?)null),
        ["TIME32"] = new Time32Type(TimeUnit.Millisecond),
        ["TIME64"] = new Time64Type(TimeUnit.Nanosecond),
        ["DURATION"] = DurationType.Nanosecond,
        ["FIXED_SIZE_BINARY"] = new FixedSizeBinaryType(4),
        ["HALF_FLOAT"] = HalfFloatType.Default,
        ["DECIMAL128"] = new Decimal128Type(10, 2),
        ["DECIMAL256"] = new Decimal256Type(40, 2),
        ["LARGE_STRING"] = LargeStringType.Default,
        ["LARGE_BINARY"] = LargeBinaryType.Default,
        ["STRING_VIEW"] = StringViewType.Default,
        ["BINARY_VIEW"] = BinaryViewType.Default,
        ["INTERVAL_MONTHS"] = new IntervalType(IntervalUnit.YearMonth),
        ["INTERVAL_DAY_TIME"] = new IntervalType(IntervalUnit.DayTime),
        ["INTERVAL_MONTH_DAY_NANO"] = new IntervalType(IntervalUnit.MonthDayNanosecond),
        ["STRUCT"] = new StructType(new List<Field> { new("x", Int32Type.Default, true) }),
        ["LIST"] = new ListType(Int32Type.Default),
        ["LARGE_LIST"] = new LargeListType(Int32Type.Default),
        ["FIXED_SIZE_LIST"] = new FixedSizeListType(Int32Type.Default, 2),
        ["MAP"] = new MapType(StringType.Default, Int32Type.Default),
        ["SPARSE_UNION"] = new UnionType(new[] { new Field("x", Int32Type.Default, true) }, new[] { 0 }, UnionMode.Sparse),
        ["DENSE_UNION"] = new UnionType(new[] { new Field("x", Int32Type.Default, true) }, new[] { 0 }, UnionMode.Dense),
    };

    [Fact]
    public void TheEnumEqualsTheTypeScriptEnumNameForNameAndNumberForNumber()
    {
        string source = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "tables", "wire-types.ts"));
        Match body = Regex.Match(source, @"export enum WireTypeId \{(?<body>.*?)\}", RegexOptions.Singleline);
        Assert.True(body.Success, "wire-types.ts has no WireTypeId enum");

        var typescript = new SortedDictionary<string, int>(StringComparer.Ordinal);
        foreach (Match m in Regex.Matches(body.Groups["body"].Value, @"(?<name>[A-Z0-9_]+)\s*=\s*0x(?<hex>[0-9a-fA-F]+)"))
        {
            typescript[m.Groups["name"].Value.Replace("_", string.Empty, StringComparison.Ordinal)] =
                int.Parse(m.Groups["hex"].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        }

        var csharp = new SortedDictionary<string, int>(StringComparer.Ordinal);
        foreach (WireTypeId id in Enum.GetValues<WireTypeId>())
        {
            csharp[id.ToString().ToUpperInvariant()] = (int)id;
        }

        Assert.True(typescript.Count > 30, "parsed only " + typescript.Count + " TypeScript members");
        Assert.Equal(typescript, csharp);
    }

    [Fact]
    public void EveryArrowTypeMapsToTheGatewaysNumber()
    {
        string source = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "tables", "schema_codec.cpp"));
        Match body = Regex.Match(source, @"int NanoarrowTypeToWireType\(enum ArrowType type\) \{(?<body>.*?)\n\}", RegexOptions.Singleline);
        Assert.True(body.Success, "schema_codec.cpp has no NanoarrowTypeToWireType");

        var gateway = new SortedDictionary<string, int>(StringComparer.Ordinal);
        foreach (Match m in Regex.Matches(body.Groups["body"].Value, @"case NANOARROW_TYPE_(?<name>\w+):\s*return 0x(?<hex>[0-9A-Fa-f]+);"))
        {
            gateway[m.Groups["name"].Value] = int.Parse(m.Groups["hex"].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        }

        // Every arm of the gateway's switch has an Arrow type here, and no more: an arm
        // added there fails this until the C# table is checked against it.
        Assert.Equal(gateway.Keys, new SortedSet<string>(ArrowFor.Keys, StringComparer.Ordinal));
        foreach ((string name, int number) in gateway)
        {
            Assert.True(number == (int)WireTypes.Of(ArrowFor[name]), name + ": the gateway says 0x" + number.ToString("X2", CultureInfo.InvariantCulture) +
                ", C# says 0x" + ((int)WireTypes.Of(ArrowFor[name])).ToString("X2", CultureInfo.InvariantCulture));
        }
    }

    [Fact]
    public void ATypeTheGatewayHasNoNumberForIsUnknown()
    {
        Assert.Equal(WireTypeId.Unknown, WireTypes.Of(NullType.Default));
    }
}
