// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Entry point: validate the wire bytes, then run the arms.
//
// ── Why the in-process toolchain ────────────────────────────────────────────
// BenchmarkDotNet's default toolchain rebuilds the benchmark in a generated
// project and runs it as a child process. That child is built WITHOUT the
// `-p:FletcherNativeShim=` property, so nothing stages the shim beside it and the
// first P/Invoke fails. In-process runs the arms in this process, where the shim
// was staged by this project's own build and where `NativeAllocs` can hand its
// samples to the summary column. The isolation given up is a clean process per
// arm, which matters for JIT and GC state far less than for what is measured
// here: a ~200 ns native call dominated by native work.
using System;

using BenchmarkDotNet.Configs;
using BenchmarkDotNet.Diagnosers;
using BenchmarkDotNet.Jobs;
using BenchmarkDotNet.Running;
using BenchmarkDotNet.Toolchains.InProcess.Emit;

using Eiva.Fletcher;
using Eiva.Fletcher.Benchmarks;

Validation.Run();
Console.WriteLine(NativeAllocs.Available
    ? "allocation counter: present (Native allocs/row reported)"
    : "allocation counter: absent (managed allocations only; see c-abi/benchmarks/alloc_count.c)");

// `--counts-only`: the Linux leg of D-BIND-48. Runs each arm's setup - which is
// where NativeAllocs samples - prints both counts and exits, without timing
// anything. Timings are taken on Windows; the counts are what Linux is for.
if (Array.IndexOf(args, "--counts-only") >= 0)
{
    NativeAllocs.CountsOnly = true;
    var arms = new (string Name, Action<PublishBenchmarks> Setup)[]
    {
        (nameof(PublishBenchmarks.M1_FusedPerRow), b => b.SetupM1()),
        (nameof(PublishBenchmarks.M2_OneRowBatchPerPublish), b => b.SetupM2()),
        (nameof(PublishBenchmarks.M2a_BuildOneRowBatchOnly), b => b.SetupM2a()),
        (nameof(PublishBenchmarks.M2b_BuildAndExportOnly), b => b.SetupM2b()),
        (nameof(PublishBenchmarks.M3_BatchPerRow), b => b.SetupM3()),
        (nameof(PublishBenchmarks.M4_RowWriterFloor), b => b.SetupM4()),
    };
    foreach ((string name, Action<PublishBenchmarks> setup) in arms)
    {
        var benchmarks = new PublishBenchmarks();
        setup(benchmarks);
        benchmarks.Cleanup();
        Console.WriteLine(NativeAllocs.Report(name));
    }

    return;
}

IConfig config = DefaultConfig.Instance
    .AddJob(Job.Default.WithToolchain(InProcessEmitToolchain.Instance))
    .AddDiagnoser(MemoryDiagnoser.Default)
    .AddColumn(new NativeAllocsColumn());

BenchmarkSwitcher.FromTypes([typeof(PublishBenchmarks)]).Run(args, config);

namespace Eiva.Fletcher.Benchmarks
{
    /// <summary>Refuse to time arms that do not put the canonical bytes on the wire.</summary>
    internal static class Validation
    {
        internal static void Run()
        {
            using PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
            using var publisher = new Publisher(provider);
            using var subscriber = new Subscriber(provider);
            using var codec = new FletcherCodec(Telemetry.Schema);

            byte[]? captured = null;
            SubscribeResult subscription = subscriber.Subscribe(
                Telemetry.Topic, (row, _, _) => captured = row.ToArray());
            using (subscription.Subscription)
            using (subscription.Schema)
            {
                publisher.CreateTopic(Telemetry.Topic, Telemetry.Schema);

                using (Apache.Arrow.RecordBatch batch = Telemetry.Batch(1))
                using (BoundRows rows = codec.Bind(batch))
                {
                    publisher.Publish(Telemetry.Topic, rows, 0);
                }

                Require("the fused path (M1-M3)", captured);

                captured = null;
                publisher.Publish(
                    Telemetry.Topic,
                    destination =>
                    {
                        Telemetry.CanonicalRow.CopyTo(destination);
                        return Telemetry.CanonicalRow.Length;
                    },
                    Telemetry.CanonicalRow.Length);
                Require("the RowWriter path (M4)", captured);
            }

            Console.WriteLine($"validation: {Telemetry.CanonicalRow.Length} bytes == bench_publish.cpp's kCanonicalRowHex");
        }

        private static void Require(string path, byte[]? captured)
        {
            if (captured is null)
            {
                Fail($"{path} delivered nothing");
            }

            string hex = Convert.ToHexStringLower(captured!);
            if (hex != Telemetry.CanonicalRowHex)
            {
                Fail($"{path} put different bytes on the wire\n  got      {hex}\n  expected {Telemetry.CanonicalRowHex}");
            }
        }

        private static void Fail(string what)
        {
            Console.Error.WriteLine($"Fletcher.Benchmarks: validation: {what}");
            Environment.Exit(2);
        }
    }
}
