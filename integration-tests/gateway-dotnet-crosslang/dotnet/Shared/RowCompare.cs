// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The C# side's field-by-field comparison of a received CompositeCoverage with the fixture row
// it was built from (BIND-8d). Shared by the test process (the C# gateway client) and the C#
// native peer. It walks the generated model's public properties, so every field of every
// message is named in a difference (`Row.Branch.Leaves[1].Label: expected "b" got "x"`) and a
// field added to coverage.proto is compared without a line here. The C++ (row_compare.hpp) and
// TypeScript (compare.ts) peers compare the same fields.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;

using Eiva.Fletcher.TestFixtures;

using Fletcher.Gen.Integration.Coverage;

namespace Eiva.Fletcher.CrossLang;

internal static class RowCompare
{
    /// <summary>Every difference between <paramref name="expected"/> and <paramref name="actual"/>.</summary>
    public static List<string> Compare(CompositeCoverage expected, CompositeCoverage actual)
    {
        var differences = new List<string>();
        Walk("Row", expected, actual, differences);
        return differences;
    }

    /// <summary>
    /// The line a peer prints for a received row: <c>RECV &lt;seq&gt; OK</c>,
    /// <c>RECV &lt;seq&gt; MISMATCH &lt;n&gt;: &lt;diffs&gt;</c>, or <c>RECV &lt;seq&gt; UNKNOWN</c>.
    /// </summary>
    public static string ReportRow(int seq, CompositeCoverage actual)
    {
        if (seq < 0 || seq >= CoverageFixture.CompositeRowCount)
        {
            return $"RECV {seq} UNKNOWN";
        }

        List<string> differences = Compare(CoverageFixture.CompositeRow(seq), actual);
        return differences.Count == 0
            ? $"RECV {seq} OK"
            : $"RECV {seq} MISMATCH {differences.Count}:" + string.Concat(differences.Select(d => $" {d};"));
    }

    private static void Walk(string path, object? expected, object? actual, List<string> differences)
    {
        if (expected is null || actual is null)
        {
            if (expected is not null || actual is not null)
            {
                differences.Add($"{path}: expected {Show(expected)} got {Show(actual)}");
            }

            return;
        }

        Type type = expected.GetType();
        if (type != actual.GetType())
        {
            differences.Add($"{path}: expected a {type.Name} got a {actual.GetType().Name}");
            return;
        }

        if (expected is byte[] expectedBytes)
        {
            if (!expectedBytes.AsSpan().SequenceEqual((byte[])actual))
            {
                differences.Add($"{path}: expected {Show(expected)} got {Show(actual)}");
            }

            return;
        }

        if (expected is IList expectedList)
        {
            var actualList = (IList)actual;
            if (expectedList.Count != actualList.Count)
            {
                differences.Add($"{path}.Count: expected {expectedList.Count} got {actualList.Count}");
                return;
            }

            for (int i = 0; i < expectedList.Count; i++)
            {
                Walk($"{path}[{i}]", expectedList[i], actualList[i], differences);
            }

            return;
        }

        if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(KeyValuePair<,>))
        {
            Walk($"{path}.Key", type.GetProperty("Key")!.GetValue(expected), type.GetProperty("Key")!.GetValue(actual), differences);
            Walk($"{path}.Value", type.GetProperty("Value")!.GetValue(expected), type.GetProperty("Value")!.GetValue(actual), differences);
            return;
        }

        if (type.IsClass && type != typeof(string))
        {
            // A generated message: every public property is one of its fields.
            foreach (PropertyInfo property in type.GetProperties(BindingFlags.Public | BindingFlags.Instance))
            {
                Walk($"{path}.{property.Name}", property.GetValue(expected), property.GetValue(actual), differences);
            }

            return;
        }

        // Scalars, enums, strings and the Model's Timestamp / Duration: value equality.
        if (!expected.Equals(actual))
        {
            differences.Add($"{path}: expected {Show(expected)} got {Show(actual)}");
        }
    }

    private static string Show(object? value) => value switch
    {
        null => "null",
        string s => $"\"{s}\"",
        byte[] b => "0x" + Convert.ToHexString(b),
        IFormattable f => f.ToString(null, CultureInfo.InvariantCulture),
        _ => value.ToString() ?? "?",
    };
}
