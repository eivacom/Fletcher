// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-9a-2: what a tester does with the preview bundle, end to end, with nothing else.
//
//   PreviewConsumer <the bundle's gateway executable> <base|server>
//
// The second argument is the configuration the program was built as, which run.sh decides: `base`
// references Eiva.Fletcher (and so Eiva.Fletcher.Interop's edge shim) and nothing more; `server`
// is the same project with Eiva.Fletcher.Server added, as a user adds it (D10C-11, d10 L5-L7).
//
// 1. Natively through Eiva.Fletcher, over `inprocess`: the generated FixFeed_Report pair
//    publishes and subscribes row by row, then SubscriberArrow receives the same rows as one
//    Arrow batch read through the generated accessor; then the pair again over `fastdds`, in
//    this one process (D-BIND-151); and the shim that answered is the one the configuration's
//    package put under runtimes/<rid>/native/ in this application's output: Eiva.Fletcher.Interop's
//    for `base`, Eiva.Fletcher.Server's for `server`. A batched subscribe works in the `server` configuration, on the server shim; in the
//    `base` configuration it is refused with the named NotSupported that says to add
//    Eiva.Fletcher.Server (d10 L6), and no server file is in the output at all (d10 L7).
// 2. Through Eiva.Fletcher.GatewayClient: the bundle's gateway is started over `inprocess`, and
//    the generated gateway pair carries the same rows across it between two clients, row by row
//    and then as one batch.
// The exit code is the verdict: 0 only when exactly the rows that were sent come back, each way.
//
// BIND-9a-4: the regions between snippet markers are shown, verbatim, in
// dotnet/README.package.md; scripts/check_readme_snippets.py keeps the two equal, so the
// README's code is code this program compiles and runs.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

using Apache.Arrow.Types;

using Eiva.Fletcher;
using Eiva.Fletcher.GatewayClient;
using Eiva.Fletcher.Model;

using Fletcher.Gen.Preview.Consumer;

if (args.Length != 2 || !File.Exists(args[0]) || args[1] is not ("base" or "server"))
{
    Console.Error.WriteLine("usage: PreviewConsumer <gateway executable> <base|server>");
    return 2;
}

bool serverBuild = args[1] == "server";

Fix[] sent = { MakeFix(1, "north"), MakeFix(2, "Æøå"), MakeFix(3, string.Empty) };
string[] expected = sent.Select(Describe).ToArray();
var failures = new List<string>();
Console.WriteLine($"PreviewConsumer ({args[1]}) on {RuntimeInformation.FrameworkDescription}, {RuntimeInformation.RuntimeIdentifier}");

// The net8.0 build must RUN on .NET 8, or it is a second .NET 10 run (review T-N1). A local
// rehearsal on a machine with only a newer runtime rolls forward and opts out.
#if NET10_0_OR_GREATER
const int TargetMajor = 10;
#else
const int TargetMajor = 8;
#endif
if (Environment.Version.Major != TargetMajor && Environment.GetEnvironmentVariable("FLETCHER_PREVIEW_ALLOW_ROLL_FORWARD") != "1")
{
    failures.Add($"built for .NET {TargetMajor} but running on .NET {Environment.Version}");
}

// ── 1. Natively, through the shim in the package ──
Check("native rows", NativeRows(sent), expected, failures);
if (serverBuild)
{
    Check("native batch", NativeBatch(sent), expected, failures);
}
else
{
    CheckBatchedSubscribeRefused(sent, failures);
}

Check("native fastdds", NativeFastDds(sent), expected, failures);

// Exactly ONE shim is loaded, and it is this configuration's own, from the package's asset (d10 L5:
// never both, never the edge shim in place of a present server shim). A check that took either
// file would tolerate that fallback.
string edgeShim = OperatingSystem.IsWindows() ? "fletcher-c-abi.dll" : "libfletcher-c-abi.so";
string serverShim = OperatingSystem.IsWindows() ? "fletcher-c-abi-server.dll" : "libfletcher-c-abi-server.so";
string shim = serverBuild ? serverShim : edgeShim;
string packaged = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "runtimes", OperatingSystem.IsWindows() ? "win-x64" : "linux-x64", "native", shim));
string[] loaded = Process.GetCurrentProcess().Modules.Cast<ProcessModule>()
    .Select(m => m.FileName)
    .Where(f => string.Equals(Path.GetFileName(f), edgeShim, StringComparison.OrdinalIgnoreCase)
        || string.Equals(Path.GetFileName(f), serverShim, StringComparison.OrdinalIgnoreCase))
    .ToArray();
Console.WriteLine("shim loaded from: " + string.Join(", ", loaded));
if (loaded.Length != 1 || !string.Equals(Path.GetFullPath(loaded[0]), packaged, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
{
    failures.Add($"exactly one shim must be loaded, {packaged}; loaded: {string.Join(", ", loaded)}");
}

// The base package's output holds no server shim anywhere (d10 L7).
if (!serverBuild)
{
    string[] serverFiles = Directory.GetFiles(AppContext.BaseDirectory, "*fletcher-c-abi-server*", SearchOption.AllDirectories);
    if (serverFiles.Length > 0)
    {
        failures.Add($"the base configuration's output holds the server shim: {string.Join(", ", serverFiles)}");
    }
}

// ── 2. Through the bundle's gateway ──
int port = FreePort();
using (Process gateway = await StartGateway(args[0], port))
{
    try
    {
        var uri = new Uri($"ws://127.0.0.1:{port}");
        Check("gateway rows", await GatewayRows(uri, sent), expected, failures);
        Check("gateway batch", await GatewayBatch(uri, sent), expected, failures);
    }
    finally
    {
        await StopGateway(gateway);
    }
}

if (failures.Count > 0)
{
    foreach (string failure in failures)
    {
        Console.Error.WriteLine("PreviewConsumer: " + failure);
    }

    return 1;
}

Console.WriteLine("PreviewConsumer: OK");
return 0;

// Row by row through the generated native pair; inprocess delivers before Publish returns.
static List<Fix> NativeRows(Fix[] fixes)
{
    var received = new List<Fix>();
    // <snippet name="native-rows">
    using PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());

    using var subscriber = new FixFeed_ReportSubscriber(provider);
    using Subscription subscription = subscriber.Subscribe((fix, attachments) => received.Add(fix));

    using var publisher = new FixFeed_ReportPublisher(provider);
    publisher.Publish(fixes);
    // </snippet>
    return received;
}

// d10 L6: on the base package a batched subscribe is refused with a named NotSupported that says
// to add Eiva.Fletcher.Server, in the shim's own words (naming fletcher-c-abi-server) plus the
// binding's sentence naming the package (ledger :575 (2)). A call that succeeds is a failure: the base configuration has no
// batches (and, with no server shim in its output, could not get any).
static void CheckBatchedSubscribeRefused(Fix[] fixes, List<string> failures)
{
    try
    {
        NativeBatch(fixes);
        failures.Add("native batch: the base configuration's batched subscribe succeeded; it must be refused (NotSupported, naming Eiva.Fletcher.Server)");
    }
    catch (FletcherException e) when (e.Status == FletcherStatus.NotSupported && e.Origin == FletcherOrigin.Seam && e.Message.Contains("Eiva.Fletcher.Server", StringComparison.Ordinal)
        && e.Message.Contains("fletcher-c-abi-server", StringComparison.Ordinal))
    {
        Console.WriteLine($"native batch: refused, as the base package must: {e.Message}");
    }
    catch (Exception e)
    {
        failures.Add($"native batch: expected FletcherException NotSupported/Seam naming Eiva.Fletcher.Server, got {e.GetType().Name}"
            + (e is FletcherException f ? $" {f.Status}/{f.Origin}" : string.Empty) + $": {e.Message}");
    }
}

// The same rows as one Arrow batch: a window of MaxRows rows, read through the generated accessor.
static List<Fix> NativeBatch(Fix[] fixes)
{
    // The window may be flushed on another thread, so the rows are handed over through a queue.
    var received = new BlockingCollection<Fix>();
    // <snippet name="native-batch">
    using PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());

    // The publisher declares the topic and its schema, which an Arrow subscriber reads rows with.
    using var publisher = new FixFeed_ReportPublisher(provider);

    using var subscriber = new SubscriberArrow(provider);
    SubscribeResult result = subscriber.SubscribeBatched(FixFeed_ReportPublisher.Topic, (batch, attachments, status) =>
    {
        using (batch)
        {
            if (FixAccessor.TryMake(batch, out FixAccessor? rows, out string? error))
            {
                for (int i = 0; i < rows.Length; ++i)
                {
                    received.Add(rows.ToRow(i)); // or rows.At(i).Label, read in place
                }
            }
        }
    }, new BatchOptions { MaxRows = fixes.Length, Timeout = TimeSpan.FromSeconds(10) });
    using Subscription subscription = result.Subscription;
    using SchemaArrival schema = result.Schema;

    publisher.Publish(fixes);
    // </snippet>
    return Take(received, fixes.Length);
}

// Row by row through the generated gateway pair, each side on its own connection.
static async Task<List<Fix>> GatewayRows(Uri uri, Fix[] fixes)
{
    var received = new BlockingCollection<Fix>();
    // <snippet name="gateway-rows">
    await using var publisherClient = new FletcherClient(new FletcherClientOptions { Uri = uri });
    await using var subscriberClient = new FletcherClient(new FletcherClientOptions { Uri = uri });
    await publisherClient.ConnectAsync();
    await subscriberClient.ConnectAsync();

    FixFeed_ReportGatewayPublisher publisher = await FixFeed_ReportGatewayPublisher.CreateAsync(publisherClient);
    var subscriber = new FixFeed_ReportGatewaySubscriber(subscriberClient);
    ulong subscription = await subscriber.SubscribeAsync((fix, envelope) => received.Add(fix));

    await publisher.PublishAsync(fixes);
    // </snippet>
    List<Fix> rows = Take(received, fixes.Length);
    await subscriber.UnsubscribeAsync(subscription);
    return rows;
}

// The same rows as one batch from the gateway, read through the generated accessor.
static async Task<List<Fix>> GatewayBatch(Uri uri, Fix[] fixes)
{
    var received = new BlockingCollection<Fix>();
    await using var publisherClient = new FletcherClient(new FletcherClientOptions { Uri = uri });
    await using var subscriberClient = new FletcherClient(new FletcherClientOptions { Uri = uri });
    await publisherClient.ConnectAsync();
    await subscriberClient.ConnectAsync();
    FixFeed_ReportGatewayPublisher publisher = await FixFeed_ReportGatewayPublisher.CreateAsync(publisherClient);
    // <snippet name="gateway-batch">
    await subscriberClient.SubscribeBatchedAsync(FixFeed_ReportGatewayPublisher.TopicKey, Fix.Schema, (batch, envelopes, status) =>
    {
        using (batch)
        {
            if (FixAccessor.TryMake(batch, out FixAccessor? rows, out string? error))
            {
                for (int i = 0; i < rows.Length; ++i)
                {
                    received.Add(rows.ToRow(i));
                }
            }
        }
    }, new GatewayBatchOptions { MaxRows = fixes.Length, Timeout = TimeSpan.FromSeconds(10) });
    // </snippet>
    await publisher.PublishAsync(fixes);
    return Take(received, fixes.Length);
}

// The rows that arrive, and then anything more that arrives within a short wait: a duplicate or
// extra delivery is returned too, so the comparison with what was sent fails (review T9).
static List<Fix> Take(BlockingCollection<Fix> received, int count)
{
    var rows = new List<Fix>();
    while (rows.Count < count && received.TryTake(out Fix? row, TimeSpan.FromSeconds(15)))
    {
        rows.Add(row);
    }

    while (received.TryTake(out Fix? extra, TimeSpan.FromMilliseconds(500)))
    {
        rows.Add(extra);
    }

    return rows;
}

// D-BIND-151: the same rows over `fastdds`, within one process on a domain of its own, so the
// statically linked eProsima chain runs from the packaged shim. Discovery is asynchronous: probe
// rows (Id 0) are published until one arrives, then the rows that are checked.
static List<Fix> NativeFastDds(Fix[] fixes)
{
    uint domain = uint.TryParse(Environment.GetEnvironmentVariable("FLETCHER_PREVIEW_DDS_DOMAIN"), out uint d) ? d : 157;
    var received = new BlockingCollection<Fix>();
    var probed = new ManualResetEventSlim();
    using PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("fastdds"), new ProviderConfig { DomainId = domain });

    using var publisher = new FixFeed_ReportPublisher(provider);
    using var subscriber = new FixFeed_ReportSubscriber(provider);
    using Subscription subscription = subscriber.Subscribe((fix, attachments) =>
    {
        if (fix.Id == 0)
        {
            probed.Set();
        }
        else
        {
            received.Add(fix);
        }
    });

    var deadline = Stopwatch.StartNew();
    while (!probed.Wait(TimeSpan.FromMilliseconds(200)) && deadline.Elapsed < TimeSpan.FromSeconds(30))
    {
        publisher.Publish(MakeFix(0, "probe"));
    }

    if (!probed.IsSet)
    {
        return [];
    }

    publisher.Publish(fixes);
    return Take(received, fixes.Length);
}

static void Check(string route, List<Fix> actual, string[] expected, List<string> failures)
{
    string[] described = actual.Select(Describe).ToArray();
    foreach (string line in described)
    {
        Console.WriteLine($"{route}: {line}");
    }

    if (!described.SequenceEqual(expected))
    {
        failures.Add($"{route}: expected{Environment.NewLine}  " + string.Join(Environment.NewLine + "  ", expected));
    }
}

static Fix MakeFix(int id, string label) => new()
{
    Id = id,
    Label = label,
    At = new Position { Lat = 55.676 + id, Lon = 12.568 - id },
    When = new Timestamp(1_700_000_000_000_000_000L + id, TimeUnit.Nanosecond),
    Readings = { 1.5 * id, -2.25, 0.0 },
    Tags = { new KeyValuePair<string, int>("hdg", 90 * id), new KeyValuePair<string, int>("sat", 12) },
};

static string Describe(Fix row) => FormattableString.Invariant(
    $"{row.Id} '{row.Label}' {row.At?.Lat}/{row.At?.Lon} {row.When.WithUnit(TimeUnit.Nanosecond).Value} [{string.Join(',', row.Readings.Select(r => r.ToString(CultureInfo.InvariantCulture)))}] {{{string.Join(',', row.Tags.Select(t => $"{t.Key}={t.Value}"))}}}");

static int FreePort()
{
    using var listener = new TcpListener(IPAddress.Loopback, 0);
    listener.Start();
    return ((IPEndPoint)listener.LocalEndpoint).Port;
}

// As integration-tests/gateway-dotnet-end-to-end starts it: ready when it prints "READY <port>",
// stopped by "stop" on its stdin.
static async Task<Process> StartGateway(string binary, int port)
{
    var start = new ProcessStartInfo(binary, $"--port {port} --bind-address 127.0.0.1 --provider inprocess")
    {
        RedirectStandardInput = true,
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false,
    };
    Process process = Process.Start(start) ?? throw new InvalidOperationException("the gateway did not start");
    _ = Task.Run(async () =>
    {
        while (await process.StandardError.ReadLineAsync() is { } line)
        {
            await Console.Error.WriteLineAsync("[gateway stderr] " + line);
        }
    });

    using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(10));
    while (true)
    {
        string? line = await process.StandardOutput.ReadLineAsync(deadline.Token);
        if (line is null)
        {
            throw new InvalidOperationException($"the gateway exited before READY (code {(process.HasExited ? process.ExitCode : -1)})");
        }

        if (line.StartsWith("READY ", StringComparison.Ordinal))
        {
            Console.WriteLine("gateway: " + line);
            break;
        }
    }

    _ = process.StandardOutput.ReadToEndAsync();
    return process;
}

static async Task StopGateway(Process process)
{
    try
    {
        await process.StandardInput.WriteLineAsync("stop");
        await process.StandardInput.FlushAsync();
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await process.WaitForExitAsync(deadline.Token);
    }
    catch (Exception e) when (e is OperationCanceledException or IOException)
    {
        process.Kill(entireProcessTree: true);
    }
}
