// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-4d-v, the managed half: the per-row publish benchmark (D-BIND-37, shape D-BIND-48).
//
// ── The arms, and the native arm each one's native half is ──────────────────
//   M1  Publish(topic, rows, i) over a pre-bound batch     native half = C2
//       The fused per-row path: P/Invoke, topic marshalling, the shim.
//   M2  one-row RecordBatch → Bind → Publish → Dispose      native half = C3
//       B-2 as the development plan writes it: a lone row paying for an Arrow
//       array, an export and a bind. This is what an application with one row's
//       values in hand pays today, and it is the number the D-BIND-1 question
//       turns on.
//   M2a M2's batch build alone, no bind, no publish          (decomposes M2)
//   M2b M2a + the C Data Interface export and release         (decomposes M2)
//   M3  Publish(topic, rows) over the batch, per row        native half = C4
//       The batch mitigation B-2 names: one crossing, `ToSegments` hoisted.
//   M4  Publish(topic, RowWriter, n) copying the canonical bytes
//       NOT an arm the bullet asks for. It is the FLOOR of the D-BIND-1
//       alternative - generated C# writing wire bytes - with the encoding itself
//       replaced by a 40-byte copy: the crossing, the writer thunk and the
//       GCHandle, and nothing a managed encoder could make cheaper. If M2 is
//       unacceptable, M4 says how much a STOP-AND-ASK could buy back at most.
//
// Every arm publishes the canonical row (Telemetry.cs) to the generated C++
// publisher's topic, over `inprocess`, with no subscriber - as the C++ arms do.
// Program.cs validates the bytes before anything is timed.
using System;

using Apache.Arrow;
using Apache.Arrow.C;

using BenchmarkDotNet.Attributes;

namespace Eiva.Fletcher.Benchmarks;

public class PublishBenchmarks
{
    /// <summary>Rows in the pre-bound batch: the same 1024 bench_publish.cpp uses.</summary>
    public const int BatchRows = 1024;

    private PubSubProviderHandle _provider = null!;
    private Publisher _publisher = null!;
    private FletcherCodec _codec = null!;
    private RecordBatch _batch = null!;
    private BoundRows _bound = null!;
    private RowWriter _writer = null!;
    private int _next;

    /// <summary><c>ArrowArray.release</c>: after five <c>int64_t</c> and three pointers.</summary>
    private static readonly int ReleaseOffset = (5 * sizeof(long)) + (3 * IntPtr.Size);

    [GlobalSetup(Target = nameof(M1_FusedPerRow))]
    public void SetupM1() => Setup(nameof(M1_FusedPerRow), M1_FusedPerRow, 1);

    [GlobalSetup(Target = nameof(M2_OneRowBatchPerPublish))]
    public void SetupM2() => Setup(nameof(M2_OneRowBatchPerPublish), M2_OneRowBatchPerPublish, 1);

    [GlobalSetup(Target = nameof(M2a_BuildOneRowBatchOnly))]
    public void SetupM2a() => Setup(nameof(M2a_BuildOneRowBatchOnly), M2a_BuildOneRowBatchOnly, 1);

    [GlobalSetup(Target = nameof(M2b_BuildAndExportOnly))]
    public void SetupM2b() => Setup(nameof(M2b_BuildAndExportOnly), M2b_BuildAndExportOnly, 1);

    [GlobalSetup(Target = nameof(M3_BatchPerRow))]
    public void SetupM3() => Setup(nameof(M3_BatchPerRow), M3_BatchPerRow, BatchRows);

    [GlobalSetup(Target = nameof(M4_RowWriterFloor))]
    public void SetupM4() => Setup(nameof(M4_RowWriterFloor), M4_RowWriterFloor, 1);

    [GlobalCleanup]
    public void Cleanup()
    {
        _bound.Dispose();
        _batch.Dispose();
        _codec.Dispose();
        _publisher.Dispose();
        _provider.Dispose();
    }

    [Benchmark(Baseline = true)]
    public void M1_FusedPerRow()
    {
        _publisher.Publish(Telemetry.Topic, _bound, _next);
        _next = _next + 1 == BatchRows ? 0 : _next + 1;
    }

    [Benchmark]
    public void M2_OneRowBatchPerPublish()
    {
        using RecordBatch batch = Telemetry.Batch(1);
        using BoundRows rows = _codec.Bind(batch);
        _publisher.Publish(Telemetry.Topic, rows, 0);
    }

    /// <summary>M2 with the bind and the publish taken out: the Arrow half alone.</summary>
    /// <remarks>
    /// A DECOMPOSITION of M2, not an arm of its own. M2 − M2a is the export,
    /// the bind and the publish; M2a is what building a one-row batch costs in
    /// Apache.Arrow before Fletcher is involved at all.
    /// </remarks>
    [Benchmark]
    public void M2a_BuildOneRowBatchOnly()
    {
        using RecordBatch batch = Telemetry.Batch(1);
    }

    /// <summary>M2a plus the C Data Interface export and its release: still no Fletcher.</summary>
    /// <remarks>
    /// <para>
    /// A second DECOMPOSITION of M2. It does exactly what <c>ExportedArray.Export</c>
    /// and <c>ExportedArray.ReleaseHandle</c> do - <c>CArrowArray.Create</c>,
    /// <c>CArrowArrayExporter.ExportRecordBatch</c>, the release callback called
    /// explicitly, <c>CArrowArray.Free</c> - and nothing else, so M2b − M2a is
    /// Apache.Arrow's exporter and M2 − M2b is Fletcher: the managed bind wrapper
    /// plus the native bind, publish and unbind (whose native share is C3).
    /// </para>
    /// <para>
    /// The release is read through <see cref="ReleaseOffset"/> rather than through
    /// Fletcher's <c>ArrowArrayAbi</c>, which is internal - this project uses the
    /// public surface only. The offset is the frozen C Data Interface layout: five
    /// int64 fields and three pointers precede <c>release</c>.
    /// </para>
    /// </remarks>
    [Benchmark]
    public unsafe void M2b_BuildAndExportOnly()
    {
        using RecordBatch batch = Telemetry.Batch(1);
        CArrowArray* exported = CArrowArray.Create();
        CArrowArrayExporter.ExportRecordBatch(batch, exported);

        nint release = *(nint*)((byte*)exported + ReleaseOffset);
        if (release != 0)
        {
            ((delegate* unmanaged[Cdecl]<CArrowArray*, void>)release)(exported);
        }

        CArrowArray.Free(exported);
    }

    [Benchmark(OperationsPerInvoke = BatchRows)]
    public void M3_BatchPerRow() => _publisher.Publish(Telemetry.Topic, _bound);

    [Benchmark]
    public void M4_RowWriterFloor() => _publisher.Publish(Telemetry.Topic, _writer, Telemetry.CanonicalRow.Length);

    private void Setup(string arm, Action call, int rowsPerCall)
    {
        _provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        _publisher = new Publisher(_provider);
        _codec = new FletcherCodec(Telemetry.Schema);
        _batch = Telemetry.Batch(BatchRows);
        _bound = _codec.Bind(_batch);
        _publisher.CreateTopic(Telemetry.Topic, Telemetry.Schema);

        // Cached once, so M4 measures the publish rather than a delegate allocation.
        _writer = static destination =>
        {
            Telemetry.CanonicalRow.CopyTo(destination);
            return Telemetry.CanonicalRow.Length;
        };

        NativeAllocs.Record(arm, call, rowsPerCall);
    }
}
