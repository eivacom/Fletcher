// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Native allocations per row, read from `c-abi/benchmarks/alloc_count.c`.
//
// BenchmarkDotNet's MemoryDiagnoser counts the MANAGED heap only. B-2's cost is
// half native - the shim's `ToSegments`, a bind's `fl_rows`, Apache.Arrow's
// C Data Interface structs - so that half is read from the same LD_PRELOADed
// malloc counter the C++ harness reads (D-BIND-48). ONE instrument for both
// harnesses, so the C++ and C# columns count the same thing.
//
// Linux only. On Windows the export is absent and the column reads "n/a" - never
// 0, which would read as a measurement.
using System;
using System.Collections.Concurrent;
using System.Globalization;
using System.Runtime.InteropServices;

using BenchmarkDotNet.Columns;
using BenchmarkDotNet.Reports;
using BenchmarkDotNet.Running;

namespace Eiva.Fletcher.Benchmarks;

internal static unsafe class NativeAllocs
{
    private static readonly int Warmup = 20_000;
    private static readonly int Samples = 1_000;

    private static readonly delegate* unmanaged<ulong> Counter = Resolve();

    private static readonly ConcurrentDictionary<string, double> PerRow = new();

    private static readonly ConcurrentDictionary<string, double> ManagedBytesPerRow = new();

    internal static bool Available => Counter != null;

    /// <summary>Set by <c>--counts-only</c>: sample even without the native counter, for the managed count.</summary>
    internal static bool CountsOnly { get; set; }

    /// <summary>Sample <paramref name="call"/> and record allocations per row under <paramref name="arm"/>.</summary>
    /// <remarks>
    /// Warmed well past tier-0 first: a JIT compile on the calling thread would
    /// otherwise be counted as the arm's. The counter is thread-local on the
    /// native side, so the runtime's background threads do not leak into it.
    /// </remarks>
    internal static void Record(string arm, Action call, int rowsPerCall)
    {
        if (!Available && !CountsOnly)
        {
            return;
        }

        for (int i = 0; i < Warmup; i++)
        {
            call();
        }

        long managedBefore = GC.GetAllocatedBytesForCurrentThread();
        ulong before = Available ? Counter() : 0;
        for (int i = 0; i < Samples; i++)
        {
            call();
        }

        ulong after = Available ? Counter() : 0;
        long managedAfter = GC.GetAllocatedBytesForCurrentThread();
        if (Available)
        {
            PerRow[arm] = (after - before) / ((double)Samples * rowsPerCall);
        }

        ManagedBytesPerRow[arm] = (managedAfter - managedBefore) / ((double)Samples * rowsPerCall);
    }

    /// <summary>The <c>--counts-only</c> line for one arm: both counts, no timing.</summary>
    internal static string Report(string arm) =>
        string.Create(
            CultureInfo.InvariantCulture,
            $"{arm,-26} native allocs/row {Describe(arm),8}   managed bytes/row {(ManagedBytesPerRow.TryGetValue(arm, out double bytes) ? bytes.ToString("0.#", CultureInfo.InvariantCulture) : "n/a"),8}");

    internal static string Describe(string arm) =>
        !Available ? "n/a"
        : PerRow.TryGetValue(arm, out double value) ? value.ToString("0.###", CultureInfo.InvariantCulture)
        : "?";

    private static delegate* unmanaged<ulong> Resolve()
    {
        // The main program handle searches the global scope, which is where an
        // LD_PRELOADed object's symbols live.
        return NativeLibrary.TryGetExport(NativeLibrary.GetMainProgramHandle(), "fbench_alloc_count", out nint address)
            ? (delegate* unmanaged<ulong>)address
            : null;
    }
}

/// <summary>The summary column for <see cref="NativeAllocs"/>.</summary>
internal sealed class NativeAllocsColumn : IColumn
{
    public string Id => nameof(NativeAllocsColumn);

    public string ColumnName => "Native allocs/row";

    public bool AlwaysShow => true;

    public ColumnCategory Category => ColumnCategory.Metric;

    public int PriorityInCategory => 0;

    public bool IsNumeric => true;

    public UnitType UnitType => UnitType.Dimensionless;

    public string Legend => "malloc-family calls per row on the calling thread (Linux, LD_PRELOAD alloc_count.c)";

    public string GetValue(Summary summary, BenchmarkCase benchmarkCase) =>
        NativeAllocs.Describe(benchmarkCase.Descriptor.WorkloadMethod.Name);

    public string GetValue(Summary summary, BenchmarkCase benchmarkCase, SummaryStyle style) =>
        GetValue(summary, benchmarkCase);

    public bool IsDefault(Summary summary, BenchmarkCase benchmarkCase) => false;

    public bool IsAvailable(Summary summary) => true;
}
