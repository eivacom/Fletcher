// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D10C-2: every number the shim hands a C# app means the same in C# as in the C header.
//
// The header (`c-abi/include/fletcher/abi/binding.h`) publishes four tables of numbers
// that cross to C#: a failure's status, where it came from (origin), why a batch closed
// (reason) and which build is loaded (variant). Each has a C# mirror. This reads the
// header and requires, per table, that the set of (name, number) pairs is the same on
// both sides, so a number added, renumbered or dropped on either side fails here
// instead of reaching an app as a value with the wrong name or no name.
//
// Name rule: the header name, minus its table's prefix, in PascalCase
// (FL_NOT_SUPPORTED -> NotSupported, FL_BATCH_ROW_LIMIT -> RowLimit).
//
// The header is the repo's own, staged beside the test binary by the csproj. A missing
// header FAILS the test; it never skips, so "the header was not there" cannot read green.
//
// The reader REFUSES what it does not understand: an enumerator with no explicit value, a
// formula value, or a #define whose value is not a bare integer FAILS the test. A reader
// that skipped such an entry would let the header gain a member the C# side also lacks,
// and the table would still read equal. Decimal and 0x values are parsed.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

using Eiva.Fletcher.Interop;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class AbiNumberTableTests
{
    private const string HeaderFile = "binding.h";

    /// <summary>One row per table: the C header's table against its C# mirror.</summary>
    /// <remarks>
    /// The status row is a declared guard: it is green before and after D10C-2, and it is
    /// here so that no later stage can break the status numbers unseen. The other three
    /// rows are the ones D10C-2 turns green.
    /// </remarks>
    [Theory]
    [InlineData("fl_status")]
    [InlineData("fl_origin")]
    [InlineData("FL_BATCH_*")]
    [InlineData("fl_variant")]
    public void EveryNumberTheShimHandsOutHasTheSameValueInCSharp(string table)
    {
        string header = StripComments(ReadHeader());

        (Dictionary<string, long> fromHeader, Type mirror) = table switch
        {
            "fl_status" => (ReadEnum(header, "fl_status", "FL_"), typeof(FletcherStatus)),
            "fl_origin" => (ReadEnum(header, "fl_origin", "FL_ORIGIN_"), typeof(FletcherOrigin)),
            "FL_BATCH_*" => (ReadDefines(header, "FL_BATCH_"), typeof(BatchReason)),
            "fl_variant" => (ReadEnum(header, "fl_variant", "FL_VARIANT_"), typeof(FlVariant)),
            _ => throw new ArgumentOutOfRangeException(nameof(table), table, "no such table row"),
        };

        // A table that parsed to nothing would pass the comparison below vacuously.
        Assert.True(fromHeader.Count > 0, $"the header's {table} table parsed to no members: the reader is wrong, not the C#");

        Dictionary<string, long> fromCSharp = Enum.GetNames(mirror)
            .ToDictionary(name => name, name => Convert.ToInt64(Enum.Parse(mirror, name)), StringComparer.Ordinal);

        string[] headerOnly = fromHeader.Except(fromCSharp).Select(Describe).OrderBy(s => s, StringComparer.Ordinal).ToArray();
        string[] csharpOnly = fromCSharp.Except(fromHeader).Select(Describe).OrderBy(s => s, StringComparer.Ordinal).ToArray();

        Assert.True(
            headerOnly.Length == 0 && csharpOnly.Length == 0,
            $"{table} and {mirror.Name} differ. In the header only (name=number): [{string.Join(", ", headerOnly)}]. " +
            $"In C# only: [{string.Join(", ", csharpOnly)}].");
    }

    /// <summary>`fl_binding_variant` on the staged library answers a defined `FlVariant` member.</summary>
    /// <remarks>
    /// A leg, not the value proof: it shows the import exists and resolves in the shim the
    /// tests run against. Which member it answers depends on the build staged, so only
    /// "defined" is asserted; which build loads is proven from outside at rung 4.
    /// </remarks>
    [Fact]
    public void BindingVariantAnswersADefinedMember()
    {
        FlVariant answer = NativeMethods.fl_binding_variant();

        Assert.True(Enum.IsDefined(answer), $"fl_binding_variant answered {(long)answer}, which is not a member of FlVariant");
    }

    private static string Describe(KeyValuePair<string, long> member) => $"{member.Key}={member.Value}";

    private static string ReadHeader()
    {
        string path = Path.Combine(AppContext.BaseDirectory, HeaderFile);
        Assert.True(File.Exists(path), $"the C header was not staged beside the test binary ({path}): check the csproj item for binding.h");
        return File.ReadAllText(path);
    }

    private static string StripComments(string text) =>
        Regex.Replace(Regex.Replace(text, @"/\*.*?\*/", " ", RegexOptions.Singleline), @"//[^\r\n]*", " ");

    /// <summary>The members of `typedef enum <paramref name="tag"/> { ... } tag;`, keyed by PascalCase name.</summary>
    /// <remarks>Every entry must be `NAME = integer`; anything else fails rather than being skipped.</remarks>
    private static Dictionary<string, long> ReadEnum(string header, string tag, string prefix)
    {
        Match body = Regex.Match(header, $@"typedef\s+enum\s+{tag}\s*\{{(?<body>[^}}]*)\}}\s*{tag}\s*;");
        Assert.True(body.Success, $"the header has no `typedef enum {tag}`");

        var members = new Dictionary<string, long>(StringComparer.Ordinal);
        foreach (string raw in body.Groups["body"].Value.Split(','))
        {
            string entry = raw.Trim();
            if (entry.Length == 0)
            {
                continue; // after the last member's trailing comma
            }

            Match m = Regex.Match(entry, @"^(?<name>[A-Z][A-Z0-9_]*)\s*=\s*(?<value>-?(?:0[xX][0-9a-fA-F]+|\d+))$");
            if (!m.Success)
            {
                Assert.Fail($"{tag}: cannot read the entry `{entry}`; the reader takes only `NAME = integer`, so teach it this form or write the entry that way");
            }

            members.Add(Pascal(m.Groups["name"].Value, prefix), ParseInteger(m.Groups["value"].Value));
        }

        return members;
    }

    /// <summary>The `#define <paramref name="prefix"/>NAME number` lines, keyed by PascalCase name.</summary>
    /// <remarks>Every such #define must carry a bare integer; a parenthesised, cast or computed value fails.</remarks>
    private static Dictionary<string, long> ReadDefines(string header, string prefix)
    {
        var members = new Dictionary<string, long>(StringComparer.Ordinal);
        foreach (Match m in Regex.Matches(header, $@"^[ \t]*#define[ \t]+(?<name>{prefix}\w*)(?<rest>[^\r\n]*)", RegexOptions.Multiline))
        {
            string rest = m.Groups["rest"].Value.Trim();
            if (!Regex.IsMatch(rest, @"^-?(?:0[xX][0-9a-fA-F]+|\d+)$"))
            {
                Assert.Fail($"{prefix}*: cannot read `#define {m.Groups["name"].Value} {rest}`; the reader takes only a bare integer, so teach it this form or write the value that way");
            }

            members.Add(Pascal(m.Groups["name"].Value, prefix), ParseInteger(rest));
        }

        return members;
    }

    private static long ParseInteger(string text)
    {
        bool negative = text.StartsWith('-');
        string digits = negative ? text[1..] : text;
        long value = digits.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? Convert.ToInt64(digits[2..], 16)
            : long.Parse(digits, System.Globalization.CultureInfo.InvariantCulture);
        return negative ? -value : value;
    }

    private static string Pascal(string cName, string prefix)
    {
        Assert.StartsWith(prefix, cName, StringComparison.Ordinal);
        return string.Concat(
            cName[prefix.Length..]
                .Split('_', StringSplitOptions.RemoveEmptyEntries)
                .Select(word => char.ToUpperInvariant(word[0]) + word[1..].ToLowerInvariant()));
    }
}
