// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6b: every generated `Schema` is the schema C++ generates.
//
// The oracle is the .ipc file the SAME protoc run wrote with --fletcher_opt=ipc. The
// plugin builds it with its in-process nanoarrow sink, while the C# `Schema` comes from
// CsSchemaSink; SchemaVisitor drives both. So this compares two renderings of one walk,
// and it is the check that the C# sink reproduces nanoarrow's own defaults (a list's
// "item", a map's "entries", "key" and "value") rather than our idea of them.
//
// Compared all the way down: names, type ids, nullability, a timestamp's unit and
// timezone, a duration's unit, and metadata IN ORDER. Arrow metadata is an ordered list,
// so a crossing that rebuilt it from a dictionary would reorder it and still compare
// equal under any check that sorts.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;

using Apache.Arrow;
using Apache.Arrow.Ipc;
using Apache.Arrow.Types;

using Fletcher.Gen.Integration.ProtocDotnet;

using Xunit;

namespace Eiva.Fletcher.ProtocDotnet;

public sealed class GeneratedSchemaTests
{
    public static TheoryData<string> Messages =>
        new() { "Reading", "Player", "Player_Stats", "Timed" };

    [Theory]
    [MemberData(nameof(Messages))]
    public void GeneratedSchemaEqualsTheIpcSchemaThePluginWrites(string message)
    {
        Schema generated = message switch
        {
            "Reading" => Reading.Schema,
            "Player" => Player.Schema,
            "Player_Stats" => Player_Stats.Schema,
            "Timed" => Timed.Schema,
            _ => throw new ArgumentOutOfRangeException(nameof(message)),
        };
        Schema ipc = ReadIpcSchema($"model.{message}.ipc");

        var differences = new List<string>();
        Compare(Metadata(generated.Metadata), Metadata(ipc.Metadata), $"{message} (schema metadata)", differences);
        Assert.Equal(ipc.FieldsList.Count, generated.FieldsList.Count);
        for (int i = 0; i < ipc.FieldsList.Count; i++)
            CompareField(generated.FieldsList[i], ipc.FieldsList[i], $"{message}.{ipc.FieldsList[i].Name}", differences);

        Assert.True(differences.Count == 0, string.Join(Environment.NewLine, differences));
    }

    [Fact]
    public void TheComparisonCanFail()
    {
        // A comparison that has never failed proves nothing. Change one thing the
        // generated schema could plausibly get wrong, and require it to be reported.
        Field scores = Player.Schema.GetFieldByName("scores");
        var list = (ListType)scores.DataType;
        var wrongItem = new Field(list.ValueField.Name, list.ValueField.DataType, !list.ValueField.IsNullable);
        var wrong = new Field(scores.Name, new ListType(wrongItem), scores.IsNullable, scores.Metadata);

        var differences = new List<string>();
        CompareField(wrong, scores, "scores", differences);

        Assert.Contains(differences, d => d.Contains("scores/item: nullable", StringComparison.Ordinal));
    }

    [Fact]
    public void TheIpcFilesExistForEveryMessageWithASchema()
    {
        // A missing .ipc would make the theory above fail on a file error, which reads
        // like a generator fault. Say what is missing instead.
        foreach (var name in new[] { "Reading", "Player", "Player_Stats", "Timed" })
            Assert.True(File.Exists(Path.Combine(GenDir(), $"model.{name}.ipc")), $"model.{name}.ipc was not generated");
    }

    [Fact]
    public void ModelOnlyLeavesOutTheNativePairFile()
    {
        // D-BIND-76: `csharp` writes the native pair beside the model unless this project's
        // `csharp_model_only` opts out, which is how an Apache.Arrow-only consumer keeps
        // every Eiva.Fletcher type out of its build.
        Assert.True(File.Exists(Path.Combine(GenDir(), "model.fletcher.cs")));
        Assert.Empty(Directory.GetFiles(GenDir(), "*.fletcher.native.cs"));
    }

    private static void CompareField(Field generated, Field ipc, string path, List<string> differences)
    {
        if (generated.Name != ipc.Name) differences.Add($"{path}: name '{generated.Name}' vs '{ipc.Name}'");
        if (generated.IsNullable != ipc.IsNullable)
            differences.Add($"{path}: nullable {generated.IsNullable} vs {ipc.IsNullable}");
        Compare(Metadata(generated.HasMetadata ? generated.Metadata : null),
                Metadata(ipc.HasMetadata ? ipc.Metadata : null), $"{path} (metadata)", differences);
        CompareType(generated.DataType, ipc.DataType, path, differences);
    }

    private static void CompareType(IArrowType generated, IArrowType ipc, string path, List<string> differences)
    {
        if (generated.TypeId != ipc.TypeId)
        {
            differences.Add($"{path}: type {generated.TypeId} vs {ipc.TypeId}");
            return;
        }
        switch (generated)
        {
            case TimestampType g:
                var t = (TimestampType)ipc;
                if (g.Unit != t.Unit || g.Timezone != t.Timezone)
                    differences.Add($"{path}: timestamp {g.Unit}/{g.Timezone} vs {t.Unit}/{t.Timezone}");
                break;
            case DurationType g:
                if (g.Unit != ((DurationType)ipc).Unit) differences.Add($"{path}: duration unit {g.Unit}");
                break;
            case NestedType g:
                var n = (NestedType)ipc;
                if (g.Fields.Count != n.Fields.Count)
                {
                    differences.Add($"{path}: {g.Fields.Count} vs {n.Fields.Count} children");
                    break;
                }
                for (int i = 0; i < g.Fields.Count; i++)
                    CompareField(g.Fields[i], n.Fields[i], $"{path}/{n.Fields[i].Name}", differences);
                break;
        }
    }

    private static void Compare(string generated, string ipc, string path, List<string> differences)
    {
        if (generated != ipc) differences.Add($"{path}: [{generated}] vs [{ipc}]");
    }

    private static string Metadata(IReadOnlyDictionary<string, string>? metadata) =>
        metadata is null ? "" : string.Join(", ", metadata.Select(kv => kv.Key + "=" + kv.Value));

    private static string GenDir() =>
        typeof(GeneratedSchemaTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
            .Single(a => a.Key == "FletcherGenDir").Value!;

    // A schema-only IPC stream: asking for a batch is how the reader reaches the schema,
    // and the null it answers with is the expected outcome (as SchemaIpcTests reads them).
    private static Schema ReadIpcSchema(string fileName)
    {
        using FileStream file = File.OpenRead(Path.Combine(GenDir(), fileName));
        using var reader = new ArrowStreamReader(file);
        reader.ReadNextRecordBatch()?.Dispose();
        return reader.Schema;
    }
}
