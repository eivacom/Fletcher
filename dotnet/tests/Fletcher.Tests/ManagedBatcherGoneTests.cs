// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D10C-7, ladder rung 8 (merged by ruling :660): C#'s own batcher is gone.
//
// d10 L4: "There is one batch assembler, and it is not in C#. The binding holds no
// window, row count, deadline, timer or batch thread of its own." Register BC-9's
// structural test, scoped binding-wide by design review C2-1: a batcher re-grown in
// another file or type must fail here, not only one inside SubscriberArrow.
//
// Two scans, each with controls (a scan that reads nothing finds nothing):
//   * the ASSEMBLIES, by metadata: no timer type referenced anywhere in Fletcher.dll;
//     Stopwatch used by no type but SchemaArrival (its Wait clock, not a batch
//     deadline); no `fl_decode_rows_framed` import in Fletcher.Interop.dll. The input
//     is the build output, or - when the pack step hands it in through
//     FLETCHER_PACKED_ASSEMBLIES - the assemblies unzipped from the packed .nupkgs, so
//     the same test proves what ships. Never skipped: absent input fails.
//   * the SOURCE of dotnet/src/Fletcher, code lines only: no timer, no Stopwatch outside
//     SchemaArrival.cs, no held row buffer and no framed window decode.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Text.RegularExpressions;

using Eiva.Fletcher.GatewayClient;
using Eiva.Fletcher.Interop;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class ManagedBatcherGoneTests
{
    /// <summary>The pack step's output: a folder holding the unzipped .nupkg contents.</summary>
    internal const string PackedVariable = "FLETCHER_PACKED_ASSEMBLIES";

    /// <summary>The one type allowed a Stopwatch (C2-1): a schema wait's clock, not a batch deadline.</summary>
    private static readonly string[] StopwatchAllowed = ["Eiva.Fletcher.SchemaArrival"];

    /// <summary>
    /// d10 L4 by metadata (ladder rung 8; BC-9 narrowed; C2-1): in every Fletcher.dll given, no
    /// type references a timer and no type but <see cref="SchemaArrival"/> uses
    /// <c>System.Diagnostics.Stopwatch</c>; in every Fletcher.Interop.dll, no
    /// <c>fl_decode_rows_framed</c> import (d9 keeps the entry point, d10 Q8).
    /// </summary>
    /// <remarks>
    /// Controls, each the same detector fed something that holds its pattern: the Gateway client
    /// (domain 5's own batcher, out of scope) references a Timer and uses a Stopwatch; the input's
    /// own SchemaArrival uses a Stopwatch; the input's Interop imports <c>fl_decode_rows</c>.
    /// </remarks>
    [Fact]
    public void TheManagedBatcherIsGoneFromTheAssemblies()
    {
        (string[] fletcher, string[] interop, string from) = Inputs();
        Assert.True(fletcher.Length > 0, $"no Fletcher.dll in {from}");
        Assert.True(interop.Length > 0, $"no Fletcher.Interop.dll in {from}");

        // Controls: the detectors find what holds their pattern.
        string gateway = typeof(GatewayBatchOptions).Assembly.Location;
        Assert.NotEmpty(TimerReferences(gateway));
        Assert.Contains("Eiva.Fletcher.GatewayClient.GatewayBatcher", StopwatchUsers(gateway));

        foreach (string assembly in fletcher)
        {
            string[] timers = TimerReferences(assembly);
            Assert.True(timers.Length == 0, $"{assembly} references a timer: {string.Join(", ", timers)}");

            string[] users = StopwatchUsers(assembly);
            Assert.Contains("Eiva.Fletcher.SchemaArrival", users);  // control: the input is read
            string[] stray = users.Except(StopwatchAllowed, StringComparer.Ordinal).ToArray();
            Assert.True(stray.Length == 0, $"{assembly}: Stopwatch used outside {{SchemaArrival}} by {string.Join(", ", stray)}");
        }

        foreach (string assembly in interop)
        {
            string[] imports = Imports(assembly);
            Assert.Contains("fl_decode_rows", imports);  // control
            Assert.DoesNotContain("fl_decode_rows_framed", imports);
        }
    }

    /// <summary>
    /// d10 L4 by source (BC-9's structural test; C2-1): over the code lines of
    /// <c>dotnet/src/Fletcher</c>, no timer type, no <c>Stopwatch</c> outside
    /// <c>SchemaArrival.cs</c>, no held row buffer (an <c>ArrayBufferWriter&lt;byte&gt;</c> field)
    /// and no framed window decode.
    /// </summary>
    /// <remarks>
    /// Comment lines are skipped (as NativeLoaderTests.NoNativeLoadOutsideTheResolver does), so
    /// prose may name what the code must not hold. Controls: every pattern matches a line that
    /// holds it, and the real tree's one allowed Stopwatch is found.
    /// </remarks>
    [Fact]
    public void TheBindingSourceHoldsNoTimerClockOrRowBuffer()
    {
        var timer = new Regex(@"\b\w*Timer\b");
        var stopwatch = new Regex(@"\bStopwatch\b");
        var rowBuffer = new Regex(@"\bArrayBufferWriter<byte>\??\s+_\w+|\bDecodeBatchFramed\b|\bfl_decode_rows_framed\b");

        // Pattern controls: each fed a line that holds it.
        Assert.Matches(timer, "private Timer? _timer;");
        Assert.Matches(timer, "using var tick = new PeriodicTimer(period);");
        Assert.Matches(stopwatch, "private long _lastFlush = Stopwatch.GetTimestamp();");
        Assert.Matches(rowBuffer, "private ArrayBufferWriter<byte> _rows = new();");
        Assert.Matches(rowBuffer, "batch = codec.DecodeBatchFramed(rows.WrittenSpan, ends, valid);");

        (string File, int Line, string Text)[] code = FletcherCode();
        static bool IsSchemaArrival(string file) => Path.GetFileName(file) == "SchemaArrival.cs";
        string[] Hits(Regex pattern, Func<string, bool> fileFilter) => code
            .Where(l => fileFilter(l.File) && pattern.IsMatch(l.Text))
            .Select(l => $"{l.File}:{l.Line}: {l.Text}")
            .ToArray();

        // Tree control: the scan reads the folder, and finds the one Stopwatch it allows.
        Assert.NotEmpty(Hits(stopwatch, IsSchemaArrival));

        string[] found =
        [
            .. Hits(timer, _ => true),
            .. Hits(stopwatch, f => !IsSchemaArrival(f)),
            .. Hits(rowBuffer, _ => true),
        ];
        Assert.True(found.Length == 0, "d10 L4: dotnet/src/Fletcher holds a batcher's timer, clock or row buffer:\n" +
            string.Join("\n", found));
    }

    // ── Inputs ──────────────────────────────────────────────────────────────

    /// <summary>
    /// The pack step's folder when it hands one in (every Fletcher.dll / Fletcher.Interop.dll under
    /// it, each target framework), else the build output this test runs against.
    /// </summary>
    private static (string[] Fletcher, string[] Interop, string From) Inputs()
    {
        string? packed = Environment.GetEnvironmentVariable(PackedVariable);
        if (string.IsNullOrEmpty(packed))
        {
            return (
                [typeof(SubscriberArrow).Assembly.Location],
                [typeof(NativeMethods).Assembly.Location],
                "the build output");
        }

        Assert.True(Directory.Exists(packed), $"{PackedVariable} names '{packed}', which does not exist");
        string[] Named(string name) => Directory.EnumerateFiles(packed, name, SearchOption.AllDirectories).Order(StringComparer.Ordinal).ToArray();
        return (Named("Fletcher.dll"), Named("Fletcher.Interop.dll"), $"{PackedVariable}={packed}");
    }

    // ── The detectors ───────────────────────────────────────────────────────

    /// <summary>Every type reference whose name is a timer (System.Threading.Timer, System.Timers.Timer, PeriodicTimer, ITimer).</summary>
    private static string[] TimerReferences(string assembly)
    {
        using var pe = new PEReader(File.OpenRead(assembly));
        MetadataReader md = pe.GetMetadataReader();
        return md.TypeReferences
            .Select(h => md.GetTypeReference(h))
            .Select(t => $"{md.GetString(t.Namespace)}.{md.GetString(t.Name)}")
            .Where(name => name.StartsWith("System.", StringComparison.Ordinal) && name.EndsWith("Timer", StringComparison.Ordinal))
            .ToArray();
    }

    /// <summary>The outermost type of every method whose IL refers to System.Diagnostics.Stopwatch.</summary>
    /// <remarks>
    /// Outermost, so a nested type or a compiler-generated closure counts as the type it was
    /// written in: a lambda's Stopwatch inside SchemaArrival is SchemaArrival's.
    /// </remarks>
    private static string[] StopwatchUsers(string assembly)
    {
        using var pe = new PEReader(File.OpenRead(assembly));
        MetadataReader md = pe.GetMetadataReader();
        var users = new SortedSet<string>(StringComparer.Ordinal);
        foreach (TypeDefinitionHandle typeHandle in md.TypeDefinitions)
        {
            TypeDefinition type = md.GetTypeDefinition(typeHandle);
            foreach (MethodDefinitionHandle methodHandle in type.GetMethods())
            {
                MethodDefinition method = md.GetMethodDefinition(methodHandle);
                if (method.RelativeVirtualAddress == 0)
                {
                    continue;
                }

                byte[]? il = pe.GetMethodBody(method.RelativeVirtualAddress).GetILBytes();
                if (il is not null && Tokens(il).Any(token => IsStopwatch(md, MetadataTokens.EntityHandle(token))))
                {
                    users.Add(Outermost(md, type));
                }
            }
        }

        return [.. users];
    }

    /// <summary>The entry point name of every native import.</summary>
    private static string[] Imports(string assembly)
    {
        using var pe = new PEReader(File.OpenRead(assembly));
        MetadataReader md = pe.GetMetadataReader();
        return md.MethodDefinitions
            .Select(h => md.GetMethodDefinition(h).GetImport())
            .Where(import => !import.Name.IsNil)
            .Select(import => md.GetString(import.Name))
            .ToArray();
    }

    private static string Outermost(MetadataReader md, TypeDefinition type)
    {
        while (!type.GetDeclaringType().IsNil)
        {
            type = md.GetTypeDefinition(type.GetDeclaringType());
        }

        return $"{md.GetString(type.Namespace)}.{md.GetString(type.Name)}";
    }

    private static bool IsStopwatch(MetadataReader md, EntityHandle handle)
    {
        switch (handle.Kind)
        {
            case HandleKind.TypeReference:
                TypeReference reference = md.GetTypeReference((TypeReferenceHandle)handle);
                return md.GetString(reference.Namespace) == "System.Diagnostics" && md.GetString(reference.Name) == "Stopwatch";
            case HandleKind.MemberReference:
                return IsStopwatch(md, md.GetMemberReference((MemberReferenceHandle)handle).Parent);
            case HandleKind.MethodSpecification:
                return IsStopwatch(md, md.GetMethodSpecification((MethodSpecificationHandle)handle).Method);
            default:
                return false;
        }
    }

    /// <summary>Each instruction's operand size, keyed by its opcode value (two-byte opcodes as 0xFExx).</summary>
    private static readonly Dictionary<ushort, OperandType> OperandTypes = typeof(OpCodes)
        .GetFields(BindingFlags.Public | BindingFlags.Static)
        .Select(field => (OpCode)field.GetValue(null)!)
        .ToDictionary(op => unchecked((ushort)op.Value), op => op.OperandType);

    /// <summary>The metadata tokens a method body's IL refers to (types, members, methods, fields, ldtoken).</summary>
    private static List<int> Tokens(byte[] il)
    {
        var tokens = new List<int>();
        int i = 0;
        while (i < il.Length)
        {
            ushort code = il[i++];
            if (code == 0xFE)
            {
                code = (ushort)(0xFE00 | il[i++]);
            }

            switch (OperandTypes[code])
            {
                case OperandType.InlineNone:
                    break;
                case OperandType.ShortInlineBrTarget:
                case OperandType.ShortInlineI:
                case OperandType.ShortInlineVar:
                    i += 1;
                    break;
                case OperandType.InlineVar:
                    i += 2;
                    break;
                case OperandType.InlineBrTarget:
                case OperandType.InlineI:
                case OperandType.ShortInlineR:
                case OperandType.InlineString:
                case OperandType.InlineSig:
                    i += 4;
                    break;
                case OperandType.InlineI8:
                case OperandType.InlineR:
                    i += 8;
                    break;
                case OperandType.InlineSwitch:
                    i += 4 + (4 * BitConverter.ToInt32(il, i));
                    break;
                default: // InlineField, InlineMethod, InlineTok, InlineType
                    tokens.Add(BitConverter.ToInt32(il, i));
                    i += 4;
                    break;
            }
        }

        return tokens;
    }

    /// <summary>The code lines of every file of the baked <c>dotnet/src/Fletcher</c> folder (not bin or obj), comments skipped.</summary>
    internal static (string File, int Line, string Text)[] FletcherCode()
    {
        string src = typeof(ManagedBatcherGoneTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
            .FirstOrDefault(a => a.Key == "FletcherDotnetSrc")?.Value ?? string.Empty;
        string fletcher = Path.GetFullPath(Path.Combine(src, "Fletcher"));
        Assert.True(Directory.Exists(fletcher), $"the baked dotnet/src/Fletcher folder '{fletcher}' does not exist.");
        return Directory
            .EnumerateFiles(fletcher, "*.cs", SearchOption.AllDirectories)
            .Where(f => !Path.GetRelativePath(fletcher, f).Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                .Any(part => part is "bin" or "obj"))
            .SelectMany(f => CodeLines(f).Select(l => (File: f, l.Line, l.Text)))
            .ToArray();
    }

    /// <summary>Each line of <paramref name="file"/> that is code, numbered from 1, trimmed.</summary>
    internal static (int Line, string Text)[] CodeLines(string file) => File.ReadAllLines(file)
        .Select((text, index) => (Line: index + 1, Text: text.Trim()))
        .Where(l => l.Text.Length > 0
            && !l.Text.StartsWith("//", StringComparison.Ordinal)
            && !l.Text.StartsWith("/*", StringComparison.Ordinal)
            && !l.Text.StartsWith('*'))
        .ToArray();
}
