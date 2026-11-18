// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D10C-9, ladder rung 10: bad batch options reach C# as domain 7's own refusal, and C# checks nothing.
//
// d10 L3 ("C# does not check, clamp or cap an option"), L12 ("one place turns a TimeSpan into
// milliseconds"), ruling :536 (3) ("an out-of-range batch option raises domain 7's refusal, passed
// through as FletcherException(InvalidArgument); the 49.7-day cap goes"), register BC-8 and BC-14.
//
//   * TheTimeoutCapIsGoneFromThePublicSurface - the stage's one red-first test.
//   * EveryBatchOptionReachesDomain7AsGiven - BC-8's 6 x 7 grid from public C#, on the leg this
//     process is: the server leg refuses 27 cells with domain 7's message byte for byte and runs the
//     other 15; the edge leg gives every cell the edge NotSupported (the outside proof that C# checks
//     nothing before the shim). A declared guard.
//   * NoBatchOptionIsCheckedOrClampedInCSharp / EveryTicksToMillisecondsConversionIsOnePlace - BC-8's
//     and BC-14's source scans, each fed the deleted lines as controls. Declared guards.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text.RegularExpressions;
using System.Threading;

using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

using Delivery = Eiva.Fletcher.Tests.SubscriberArrowTests.Delivery;
using Sink = Eiva.Fletcher.Tests.SubscriberArrowTests.Sink;

namespace Eiva.Fletcher.Tests;

public sealed class BatchOptionsTests : IDisposable
{
    private static readonly Schema TwoColumns = new(
    [
        new Field("x", Int32Type.Default, nullable: true),
        new Field("name", StringType.Default, nullable: true),
    ], metadata: null);

    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly SubscriberArrow _subscriber;
    private readonly List<Sink> _sinks = [];

    public BatchOptionsTests()
    {
        _provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        _publisher = new Publisher(_provider);
        _subscriber = new SubscriberArrow(_provider);
    }

    public void Dispose()
    {
        _subscriber.Dispose();
        _publisher.Dispose();
        _provider.Dispose();
        foreach (Sink sink in _sinks)
        {
            sink.Dispose();
        }
    }

    // ── The forcing test ────────────────────────────────────────────────────

    /// <summary>
    /// BatchOptions names no timeout limit (ruling :536 (3)): a longer finite Timeout is used as given,
    /// so there is no maximum to read. Red until the member is deleted.
    /// </summary>
    /// <remarks>Control: the same lookup finds the two members that stay.</remarks>
    [Fact]
    public void TheTimeoutCapIsGoneFromThePublicSurface()
    {
        const BindingFlags all = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance;
        Assert.NotEmpty(typeof(BatchOptions).GetMember(nameof(BatchOptions.MaxRows), all));
        Assert.NotEmpty(typeof(BatchOptions).GetMember(nameof(BatchOptions.Timeout), all));

        Assert.Empty(typeof(BatchOptions).GetMember("MaxTimeout", all));
    }

    // ── The grid (rung 10; BC-8) ────────────────────────────────────────────

    private static readonly long[] RowCounts = [long.MinValue, -1, 0, 1, 2, long.MaxValue];

    /// <summary>
    /// The timeouts, each with the millisecond count that crosses, LITERALLY: -1 tick stands in for the
    /// ABI's -1 (a C# -1 ms is InfiniteTimeSpan), and floors to -1 ms rather than rounding to 0.
    /// </summary>
    private static readonly (string Name, TimeSpan Value, long Ms)[] Timeouts =
    [
        ("TimeSpan.MinValue", TimeSpan.MinValue, -922337203685478L),
        ("-1 tick", TimeSpan.FromTicks(-1), -1L),
        ("0", TimeSpan.Zero, 0L),
        ("1 ms", TimeSpan.FromMilliseconds(1), 1L),
        ("InfiniteTimeSpan", Timeout.InfiniteTimeSpan, 9223372036854775807L),
        ("100 years", TimeSpan.FromDays(36525), 3155760000000L),
        ("TimeSpan.MaxValue", TimeSpan.MaxValue, 922337203685477L),
    ];

    /// <summary>Domain 7's refusal (pubsub-arrow <c>subscriber_arrow.cpp</c>, L15), spelled out for the given values.</summary>
    private static string Domain7Refusal(long maxRows, long ms) =>
        "batch options out of range: max_rows must be >= 1 (got " + maxRows.ToString(System.Globalization.CultureInfo.InvariantCulture) +
        ") and timeout must be >= 0 ms (got " + ms.ToString(System.Globalization.CultureInfo.InvariantCulture) + " ms)";

    private TopicPath DeclaredTopic(string name)
    {
        TopicPath topic = TopicPath.Of("d10c9", "grid", name);
        _publisher.CreateTopic(topic, TwoColumns);
        return topic;
    }

    private void PublishOne(TopicPath topic, int x)
    {
        using var batch = new RecordBatch(
            TwoColumns,
            [new Int32Array.Builder().Append(x).Build(), new StringArray.Builder().Append("row").Build()],
            length: 1);
        using var codec = new FletcherCodec(TwoColumns);
        using BoundRows rows = codec.Bind(batch);
        _publisher.Publish(topic, rows, 0);
    }

    private Sink NewSink()
    {
        var sink = new Sink();
        _sinks.Add(sink);
        return sink;
    }

    /// <summary>
    /// BC-8's grid: <c>MaxRows</c> x <c>Timeout</c> = 6 x 7 = 42 cells, each given to SubscribeBatched as is.
    /// Server leg: 27 cells are refused as FletcherException(InvalidArgument, Seam) with domain 7's
    /// message byte for byte (literal millisecond values), deliver nothing, and leave the topic free;
    /// the other 15 deliver as in d9's EveryBatchOptionReachesDomain7AsGiven - a row-limit window at
    /// MaxRows = 1, a timeout window at 0 or 1 ms, and otherwise nothing within 2 s and then exactly one
    /// Closing. Edge leg: every cell gets the edge NotSupported, which is the outside proof that C#
    /// refuses nothing itself.
    /// </summary>
    /// <remarks>
    /// Control: a 10 ms subscription gives Timeout inside the same 2 s window, so "nothing delivered"
    /// is not a window too short for anything to arrive.
    /// </remarks>
    [Fact]
    public void EveryBatchOptionReachesDomain7AsGiven()
    {
        var topics = new List<TopicPath>();
        var results = new List<(long Rows, (string Name, TimeSpan Value, long Ms) Timeout, Sink Sink, SubscribeResult? Accepted, Exception? Thrown)>();
        foreach (long rows in RowCounts)
        {
            foreach ((string Name, TimeSpan Value, long Ms) timeout in Timeouts)
            {
                TopicPath topic = DeclaredTopic("cell" + topics.Count);
                topics.Add(topic);
                Sink sink = NewSink();
                SubscribeResult? accepted = null;
                Exception? thrown = Record.Exception(() =>
                {
                    accepted = _subscriber.SubscribeBatched(
                        topic, sink.Handler, new BatchOptions { MaxRows = rows, Timeout = timeout.Value });
                    accepted.Value.Schema.Dispose();
                });
                results.Add((rows, timeout, sink, accepted, thrown));
            }
        }

        if (!SubscriberArrowTests.ServerLeg)
        {
            var wrongCells = new List<string>();
            foreach (var cell in results)
            {
                string label = $"max_rows {cell.Rows}, timeout {cell.Timeout.Name}";
                if (cell.Thrown is not FletcherException { Status: FletcherStatus.NotSupported, Origin: FletcherOrigin.Seam } refused
                    || !refused.Message.StartsWith(SubscriberArrowTests.EdgeShimText, StringComparison.Ordinal)
                    || !refused.Message.Contains(SubscriberArrowTests.ServerPackage, StringComparison.Ordinal))
                {
                    wrongCells.Add($"{label}: got {cell.Thrown?.GetType().Name ?? "no exception"}: {cell.Thrown?.Message}");
                }
            }

            Assert.True(wrongCells.Count == 0, "the edge leg's cells were not all the edge NotSupported:\n" + string.Join("\n", wrongCells));
            Assert.Equal(42, results.Count);
            return;
        }

        TopicPath controlTopic = DeclaredTopic("control");
        Sink control = NewSink();
        SubscribeResult controlResult = _subscriber.SubscribeBatched(controlTopic, control.Handler, new BatchOptions { MaxRows = 100, Timeout = TimeSpan.FromMilliseconds(10) });
        controlResult.Schema.Dispose();

        foreach (TopicPath topic in topics.Append(controlTopic))
        {
            PublishOne(topic, 1);
        }

        // One observation window: wait one-sidedly for each expected delivery, then sleep out the rest of
        // the 2 s before the cells that must see nothing are judged.
        DateTime windowEnd = DateTime.UtcNow + TimeSpan.FromSeconds(2);
        TimeSpan Left() => windowEnd - DateTime.UtcNow;
        foreach (var cell in results)
        {
            if (cell.Thrown is null && (cell.Rows == 1 || cell.Timeout.Ms <= 1))
            {
                cell.Sink.WaitFor(1, Left() > TimeSpan.Zero ? Left() : TimeSpan.Zero);
            }
        }

        control.WaitFor(1, Left() > TimeSpan.Zero ? Left() : TimeSpan.Zero);
        if (Left() > TimeSpan.Zero)
        {
            Thread.Sleep(Left());
        }

        var failures = new List<string>();
        int refusedCount = 0;
        for (int i = 0; i < results.Count; i++)
        {
            var (rows, timeout, sink, accepted, thrown) = results[i];
            string label = $"max_rows {rows}, timeout {timeout.Name}";
            void Check(bool ok, string what)
            {
                if (!ok)
                {
                    failures.Add($"[{label}] {what}");
                }
            }

            List<Delivery> seen = sink.Snapshot();
            if (rows < 1 || timeout.Ms < 0)
            {
                refusedCount++;
                Check(thrown is FletcherException { Status: FletcherStatus.InvalidArgument, Origin: FletcherOrigin.Seam }
                    && thrown.GetType() == typeof(FletcherException), $"refused as {thrown?.GetType().Name ?? "no exception"}");
                Check(thrown?.Message == Domain7Refusal(rows, timeout.Ms), $"message '{thrown?.Message}'");
                Check(seen.Count == 0, "a refused subscription delivered");

                // The topic is free again: a per-row subscription, which a held batched claim would refuse.
                SubscribeResult again = _subscriber.Subscribe(topics[i], (batch, _, _) => batch?.Dispose());
                again.Schema.Dispose();
                again.Subscription.Dispose();
                continue;
            }

            Check(thrown is null, $"refused: {thrown?.Message}");
            if (rows == 1 || timeout.Ms <= 1)
            {
                BatchReason reason = rows == 1 ? BatchReason.RowLimit : BatchReason.Timeout;
                Check(seen.Count == 1 && seen[0].Status.Reason == reason && seen[0].Rows == 1,
                    $"expected one {reason} window of 1 row, saw [{string.Join(", ", seen.Select(d => $"{d.Status.Reason}/{d.Rows}"))}]");
            }
            else
            {
                Check(seen.Count == 0, "a window closed with no deadline in reach");
                accepted?.Subscription.Dispose();
                seen = sink.Snapshot();
                Check(seen.Count == 1 && seen[0].Status.Reason == BatchReason.Closing && seen[0].Rows == 1,
                    $"expected exactly one Closing of 1 row, saw [{string.Join(", ", seen.Select(d => $"{d.Status.Reason}/{d.Rows}"))}]");
            }
        }

        Assert.True(failures.Count == 0, "the grid has cells that were not delivered as given:\n" + string.Join("\n", failures));
        Assert.Equal(42, results.Count);
        Assert.Equal(27, refusedCount);
        List<Delivery> controlSeen = control.Snapshot();
        Assert.True(
            controlSeen.Count == 1 && controlSeen[0].Status.Reason == BatchReason.Timeout,
            "the control's 10 ms window did not time out inside the 2 s window");
    }

    // ── The scans (rung 10; BC-8, BC-14) ────────────────────────────────────

    private const string _optionRead = @"\.(?:MaxRows|Timeout)\b";

    /// <summary>A relational operator between operands, however the operand is named (<c>maxRows</c>, <c>options.Timeout</c>), or an <c>is &lt;</c> pattern.</summary>
    private static readonly Regex Relational = new(@"\s[<>]=?\s|\bis\s+(?:not\s+)?[<>]", RegexOptions.CultureInvariant);

    /// <summary>A clamp or bound applied to a <c>.MaxRows</c> read (the name is BatchOptions' alone, so this one is tree-wide).</summary>
    private static readonly Regex ClampedMaxRows = new(@"\bMath\.(?:Clamp|Min|Max)\s*\([^;]*\.MaxRows\b", RegexOptions.CultureInvariant);

    /// <summary>Any clamp or bound, for the two places batch options are read and held.</summary>
    private static readonly Regex AnyBound = new(@"\bMath\.(?:Clamp|Min|Max)\s*\(", RegexOptions.CultureInvariant);

    private const string _theOptionsSignature = "class BatchOptions";

    /// <summary>
    /// The lines that check or clamp a batch option: a relational operator or a <c>Math</c> bound inside
    /// <c>SubscribeBatched</c>'s body or the <c>BatchOptions</c> class (the two places the options are held
    /// and passed on), and a <c>Math</c> bound on <c>.MaxRows</c> anywhere.
    /// </summary>
    private static string[] CheckedOrClamped((string File, int Line, string Text)[] code)
    {
        (int From, int To)?[] places =
            [BodyOf(code, _theCaller, _theCallerSignature), BodyOf(code, _theCaller, _theOptionsSignature)];
        bool InAPlace((string File, int Line, string Text) l) => Path.GetFileName(l.File) == _theCaller
            && places.Any(r => r is { } b && l.Line >= b.From && l.Line <= b.To);
        return code
            .Where(l => ClampedMaxRows.IsMatch(l.Text) || (InAPlace(l) && (Relational.IsMatch(l.Text) || AnyBound.IsMatch(l.Text))))
            .Select(l => $"{l.File}:{l.Line}: {l.Text}")
            .ToArray();
    }

    /// <summary>A stand-in tree: the options class, <c>SubscribeBatched</c> holding <paramref name="line"/>, and <paramref name="elsewhere"/> in another file.</summary>
    private static (string File, int Line, string Text)[] Tree(string line, string elsewhere = "int unrelated = 0;") =>
    [
        (_theCaller, 1, "public sealed class BatchOptions"), (_theCaller, 2, "{"),
        (_theCaller, 3, "public long MaxRows { get; init; } = 8000;"), (_theCaller, 4, "}"),
        (_theCaller, 5, "public unsafe SubscribeResult SubscribeBatched("), (_theCaller, 6, "{"),
        (_theCaller, 7, line), (_theCaller, 8, "}"),
        (_theOneFile, 1, elsewhere),
    ];

    /// <summary>
    /// BC-8 by source (d10 L3; ruling :536 (3)): in <c>SubscribeBatched</c>'s body and the <c>BatchOptions</c>
    /// class of <c>dotnet/src/Fletcher</c>, no <c>&lt;</c>, <c>&gt;</c>, <c>is &lt;</c>, <c>Math.Clamp</c>,
    /// <c>Math.Min</c> or <c>Math.Max</c>, whatever the operand is named; and no such <c>Math</c> bound on a
    /// <c>.MaxRows</c> read anywhere in the folder. Equality with InfiniteTimeSpan is allowed.
    /// </summary>
    /// <remarks>
    /// Controls, on stand-in trees: each deleted or likely line is flagged inside the two places
    /// (<c>if (maxRows &lt; 1)</c>, the old cap and clamp, <c>is &lt; 1</c>); equality, lambdas and
    /// <c>delegate*</c> brackets are not; a lawful arrival-side <c>x.Timeout &lt;</c> elsewhere is not; a clamp
    /// on <c>.MaxRows</c> elsewhere is. Tree control: both places are found in the real source.
    /// </remarks>
    [Fact]
    public void NoBatchOptionIsCheckedOrClampedInCSharp()
    {
        foreach (string caught in new[]
        {
            "if (maxRows < 1)",
            "if (timeoutMs < 0)",
            "if (options.MaxRows is < 1)",
            "(options.Timeout < TimeSpan.Zero || options.Timeout > BatchOptions.MaxTimeout))",
            "int maxRows = (int)Math.Clamp(options.MaxRows, 1, int.MaxValue);",
            "long t = Math.Min(timeoutMs, 5);",
        })
        {
            Assert.True(CheckedOrClamped(Tree(caught)).Length == 1, $"not caught inside SubscribeBatched: {caught}");
        }

        foreach (string allowed in new[]
        {
            "if (options.Timeout != Timeout.InfiniteTimeSpan &&",
            "bool never = options.Timeout == Timeout.InfiniteTimeSpan;",
            "var times = list.Select(o => o.Timeout);",
            "var thunk = (delegate* unmanaged[Cdecl]<nint, FlBatch*, void>)&BatchThunk;",
        })
        {
            Assert.Empty(CheckedOrClamped(Tree(allowed)));
        }

        Assert.Empty(CheckedOrClamped(Tree("int x = 0;", elsewhere: "if (arrival.Timeout < TimeSpan.Zero)")));
        Assert.Empty(CheckedOrClamped(Tree("int x = 0;", elsewhere: "var t = new Gate<int>().Timeout;")));
        Assert.Single(CheckedOrClamped(Tree("int x = 0;", elsewhere: "int m = Math.Clamp(o.MaxRows, 1, 5);")));

        (string File, int Line, string Text)[] code = ManagedBatcherGoneTests.FletcherCode();
        Assert.NotNull(BodyOf(code, _theCaller, _theCallerSignature));
        Assert.NotNull(BodyOf(code, _theCaller, _theOptionsSignature));
        Assert.Contains(code, l => Regex.IsMatch(l.Text, _optionRead));

        string[] found = CheckedOrClamped(code);
        Assert.True(found.Length == 0, "d10 L3: dotnet/src/Fletcher checks or clamps a batch option:\n" + string.Join("\n", found));
    }

    private static readonly Regex Conversion = new(
        @"\bTicksPerMillisecond\b|\.TotalMilliseconds\b|\.TotalSeconds\b|\.Ticks\b|\.Milliseconds\b|\b10_?000\b", RegexOptions.CultureInvariant);

    private const string _theOneFile = "SchemaArrival.cs";
    private const string _theOneSignature = "static long ToNativeMilliseconds(";
    private const string _theCaller = "SubscriberArrow.cs";
    private const string _theCallerSignature = "SubscribeResult SubscribeBatched(";

    /// <summary>The first and last line number of the braced body that starts at the line holding <paramref name="signature"/>.</summary>
    private static (int From, int To)? BodyOf((string File, int Line, string Text)[] code, string file, string signature)
    {
        (string File, int Line, string Text)[] lines = code.Where(l => Path.GetFileName(l.File) == file).ToArray();
        int start = System.Array.FindIndex(lines, l => l.Text.Contains(signature, StringComparison.Ordinal));
        int depth = 0;
        bool opened = false;
        for (int i = Math.Max(start, 0); start >= 0 && i < lines.Length; i++)
        {
            foreach (char c in lines[i].Text)
            {
                depth += c == '{' ? 1 : c == '}' ? -1 : 0;
                opened |= c == '{';
            }

            if (opened && depth == 0)
            {
                return (lines[start].Line, lines[i].Line);
            }
        }

        return null;
    }

    /// <summary>Every ticks-to-milliseconds expression that is not inside the one conversion method.</summary>
    private static string[] StrayConversions((string File, int Line, string Text)[] code)
    {
        (int From, int To)? body = BodyOf(code, _theOneFile, _theOneSignature);
        return code
            .Where(l => Conversion.IsMatch(l.Text))
            .Where(l => !(Path.GetFileName(l.File) == _theOneFile && body is { } b && l.Line >= b.From && l.Line <= b.To))
            .Select(l => $"{l.File}:{l.Line}: {l.Text}")
            .ToArray();
    }

    /// <summary>Whether the body of <paramref name="signature"/> in <paramref name="file"/> calls <c>ToNativeMilliseconds(</c>.</summary>
    private static bool Calls((string File, int Line, string Text)[] code, string file, string signature)
    {
        (int From, int To)? body = BodyOf(code, file, signature);
        return body is { } b && code.Any(l =>
            Path.GetFileName(l.File) == file && l.Line >= b.From && l.Line <= b.To
            && l.Text.Contains("SchemaArrival.ToNativeMilliseconds(", StringComparison.Ordinal));
    }

    /// <summary>
    /// BC-14 by source (d10 L12): every ticks-to-milliseconds expression in <c>dotnet/src/Fletcher</c> is
    /// inside <c>SchemaArrival.ToNativeMilliseconds</c>, and <c>SubscribeBatched</c> calls it.
    /// </summary>
    /// <remarks>
    /// Controls: the pattern matches the old second conversion (<c>timeout.TotalMilliseconds</c>) and the
    /// ticks forms; the same scan fed a stray conversion outside the method, in its own file and in another,
    /// reports both and reports nothing for the one inside; the call check fails on a body without the call;
    /// and the real tree's one conversion is found, so the scan reads something.
    /// </remarks>
    [Fact]
    public void EveryTicksToMillisecondsConversionIsOnePlace()
    {
        Assert.Matches(Conversion, "double total = timeout.TotalMilliseconds;");
        Assert.Matches(Conversion, "long whole = ticks / TimeSpan.TicksPerMillisecond;");
        Assert.Matches(Conversion, "long ms = options.Timeout.Ticks / 10_000;");
        Assert.Matches(Conversion, "int ms = options.Timeout.Milliseconds;");
        Assert.Matches(Conversion, "long ms = (long)(options.Timeout.TotalSeconds * 1000);");

        (string, int, string)[] synthetic =
        [
            (_theOneFile, 1, "internal static long ToNativeMilliseconds(TimeSpan timeout)"),
            (_theOneFile, 2, "{"),
            (_theOneFile, 3, "return timeout.Ticks / TimeSpan.TicksPerMillisecond;"),
            (_theOneFile, 4, "}"),
            (_theOneFile, 5, "double total = timeout.TotalMilliseconds;"),
            ("Other.cs", 1, "long ms = options.Timeout.Ticks / 10_000;"),
        ];
        Assert.Equal(new[] { "SchemaArrival.cs:5: double total = timeout.TotalMilliseconds;", "Other.cs:1: long ms = options.Timeout.Ticks / 10_000;" },
            StrayConversions(synthetic).Select(s => s.Replace(@"\", "/")).ToArray());
        (string, int, string)[] callerWith = [(_theCaller, 1, "public unsafe SubscribeResult SubscribeBatched("), (_theCaller, 2, "{"),
            (_theCaller, 3, "long t = SchemaArrival.ToNativeMilliseconds(options.Timeout);"), (_theCaller, 4, "}")];
        (string, int, string)[] callerWithout = [(_theCaller, 1, "public unsafe SubscribeResult SubscribeBatched("), (_theCaller, 2, "{"),
            (_theCaller, 3, "long t = (long)options.Timeout.TotalMilliseconds;"), (_theCaller, 4, "}")];
        Assert.True(Calls(callerWith, _theCaller, _theCallerSignature));
        Assert.False(Calls(callerWithout, _theCaller, _theCallerSignature));

        (string File, int Line, string Text)[] code = ManagedBatcherGoneTests.FletcherCode();
        Assert.Contains(code, l => Path.GetFileName(l.File) == _theOneFile && Conversion.IsMatch(l.Text));

        string[] stray = StrayConversions(code);
        Assert.True(stray.Length == 0, "d10 L12: a ticks-to-milliseconds conversion outside SchemaArrival.ToNativeMilliseconds:\n" + string.Join("\n", stray));
        Assert.True(Calls(code, _theCaller, _theCallerSignature), "SubscribeBatched does not call SchemaArrival.ToNativeMilliseconds");
    }
}
