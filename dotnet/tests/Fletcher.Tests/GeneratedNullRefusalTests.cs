// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6 review Q10, over the coverage golden compiled into this assembly.
//
// D-BIND-92: a null the caller forces past a non-nullable annotation (`StringValue = null!`, a
// null list element, map key or value, a null collection, a null row) is refused by ToArrow with
// an ArgumentException naming the class, the property and the row, and is never written. C++'s
// row class cannot hold such a null, so before this a C# writer put a null on the wire that C++
// would read past, wrote empty bytes for a null byte[], or failed with a NullReferenceException
// from inside the generated code. A plain message field was already refused (D-BIND-83).
//
// D-BIND-93: a non-nullable Timestamp or Duration starts at zero in its column's unit, so a fresh
// row and the same row read back compare equal. Checked for every generated row class here.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher.Model;

using Fletcher.Gen.Integration.Coverage;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class GeneratedNullRefusalTests
{
    private static RecordBatch Composite(Action<CompositeCoverage> force)
    {
        var row = new CompositeCoverage();
        force(row);
        return CompositeCoverage.ToArrow(new[] { new CompositeCoverage(), row });
    }

    // Each case: the ToArrow call that meets the null, and the place the message must name.
    private static readonly Dictionary<string, (Func<object> Call, string Expected)> Cases = new()
    {
        ["string field"] = (() => ScalarCoverage.ToArrow(new[] { new ScalarCoverage(), new ScalarCoverage { StringValue = null! } }),
            "ScalarCoverage.StringValue is null in row 1"),
        ["bytes field"] = (() => ScalarCoverage.ToArrow(new[] { new ScalarCoverage { BytesValue = null! } }),
            "ScalarCoverage.BytesValue is null in row 0"),
        ["null row"] = (() => ScalarCoverage.ToArrow(new ScalarCoverage[] { new(), null! }),
            "ScalarCoverage row 1 is null"),
        ["null collection"] = (() => Composite(c => c.RepeatedScalar = null!),
            "CompositeCoverage.RepeatedScalar is null in row 1"),
        ["null map"] = (() => Composite(c => c.MapStruct = null!),
            "CompositeCoverage.MapStruct is null in row 1"),
        ["string element"] = (() => Composite(c => c.RepeatedString.Add(null!)),
            "CompositeCoverage.RepeatedString holds a null element in row 1"),
        ["bytes element"] = (() => Composite(c => c.RepeatedBytes.Add(null!)),
            "CompositeCoverage.RepeatedBytes holds a null element in row 1"),
        ["message element"] = (() => Composite(c => c.RepeatedStruct.Add(null!)),
            "CompositeCoverage.RepeatedStruct holds a null element in row 1"),
        ["map key"] = (() => Composite(c => c.MapScalar.Add(new(null!, 1))),
            "CompositeCoverage.MapScalar holds a null map key in row 1"),
        ["map value"] = (() => Composite(c => c.MapStruct.Add(new("k", null!))),
            "CompositeCoverage.MapStruct holds a null map value in row 1"),
        ["inner list"] = (() => Composite(c => c.NestedStructLists.Add(null!)),
            "CompositeCoverage.NestedStructLists holds a null element in row 1"),
        ["deep element"] = (() => Composite(c => c.Depth3StructLists.Add(new() { new() { null! } })),
            "CompositeCoverage.Depth3StructLists holds a null element in row 1"),
        // A nested message checks its own fields; its row is its place in the column being built.
        ["nested message's field"] = (() => Composite(c => c.Branch.Leaf.Label = null!),
            "Leaf.Label is null in row 1"),
    };

    public static TheoryData<string> CaseNames()
    {
        var names = new TheoryData<string>();
        foreach (string name in Cases.Keys)
        {
            names.Add(name);
        }
        return names;
    }

    [Theory]
    [MemberData(nameof(CaseNames))]
    public void AForcedNullIsRefusedWithItsPlace(string name)
    {
        var (call, expected) = Cases[name];
        ArgumentException e = Assert.Throws<ArgumentException>(call);
        Assert.Contains(expected, e.Message, StringComparison.Ordinal);
        Assert.Equal("rows", e.ParamName);
    }

    [Fact]
    public void AFreshRowsTemporalValuesStartInTheColumnsUnit()
    {
        var row = new ScalarCoverage();
        Assert.Equal(new Timestamp(0, TimeUnit.Nanosecond), row.TimestampValue);
        Assert.Equal(new Duration(0, TimeUnit.Nanosecond), row.DurationValue);
    }

    [Fact]
    public void EveryGeneratedRowReadsBackItsFreshTemporalValuesEqual()
    {
        // Every generated row class compiled here: a fresh instance converts (nothing the generator
        // initialises is refused) and each non-nullable Timestamp or Duration reads back equal.
        int checkedProperties = 0;
        foreach (Type cls in typeof(GeneratedNullRefusalTests).Assembly.GetTypes()
                     .Where(t => t.IsClass && t.Namespace?.StartsWith("Fletcher.Gen", StringComparison.Ordinal) == true
                         && t.GetMethod("FromArrow", BindingFlags.Public | BindingFlags.Static) is not null
                         && t.GetConstructor(Type.EmptyTypes) is not null))
        {
            object sent = Activator.CreateInstance(cls)!;
            var rows = System.Array.CreateInstance(cls, 1);
            rows.SetValue(sent, 0);
            using var batch = (RecordBatch)cls.GetMethod("ToArrow", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, new object[] { rows })!;
            var array = new StructArray(new StructType(batch.Schema.FieldsList), batch.Length, batch.Arrays, ArrowBuffer.Empty, 0);
            object back = cls.GetMethod("FromArrow", BindingFlags.Public | BindingFlags.Static)!.Invoke(null, new object[] { array, 0 })!;
            foreach (PropertyInfo p in cls.GetProperties(BindingFlags.Public | BindingFlags.Instance)
                         .Where(p => p.PropertyType == typeof(Timestamp) || p.PropertyType == typeof(Duration)))
            {
                Assert.True(Equals(p.GetValue(sent), p.GetValue(back)), $"{cls.Name}.{p.Name}: {p.GetValue(sent)} became {p.GetValue(back)}");
                checkedProperties++;
            }
        }
        Assert.True(checkedProperties >= 2, $"only {checkedProperties} temporal properties found");
    }
}
