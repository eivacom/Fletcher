// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-9a-2: what a tester does with the preview bundle, end to end, with nothing else.
//
//   PreviewConsumer <the bundle's gateway executable>
//
// 1. Natively through Eiva.Fletcher: the generated FixFeed_Report pair publishes and subscribes
//    over `inprocess`, and the shim that answered is the one Eiva.Fletcher.Interop's package
//    put under runtimes/<rid>/native/ in this application's output.
// 2. Through Eiva.Fletcher.GatewayClient: the bundle's gateway is started over `inprocess`, and
//    the generated gateway pair carries the same rows across it between two clients.
// The exit code is the verdict: 0 only when exactly the rows that were sent come back, twice.
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

if (args.Length != 1 || !File.Exists(args[0]))
{
    Console.Error.WriteLine("usage: PreviewConsumer <gateway executable>");
    return 2;
}

Fix[] sent = { MakeFix(1, "north"), MakeFix(2, "Æøå"), MakeFix(3, string.Empty) };
string[] expected = sent.Select(Describe).ToArray();
var failures = new List<string>();
Console.WriteLine($"PreviewConsumer on {RuntimeInformation.FrameworkDescription}, {RuntimeInformation.RuntimeIdentifier}");

// ── 1. Natively, through the shim in the package ──
using (PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig()))
{
    var native = new List<string>();
    using var subscriber = new FixFeed_ReportSubscriber(provider);
    using (Subscription subscription = subscriber.Subscribe((row, attachments) => native.Add(Describe(row))))
    using (var publisher = new FixFeed_ReportPublisher(provider))
    {
        publisher.Publish(sent);
    }

    Check("native", native, expected, failures);
}

string shim = OperatingSystem.IsWindows() ? "fletcher-c-abi.dll" : "libfletcher-c-abi.so";
string packaged = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "runtimes", OperatingSystem.IsWindows() ? "win-x64" : "linux-x64", "native", shim));
string[] loaded = Process.GetCurrentProcess().Modules.Cast<ProcessModule>()
    .Select(m => m.FileName)
    .Where(f => string.Equals(Path.GetFileName(f), shim, StringComparison.OrdinalIgnoreCase))
    .ToArray();
Console.WriteLine("shim loaded from: " + string.Join(", ", loaded));
if (loaded.Length != 1 || !string.Equals(Path.GetFullPath(loaded[0]), packaged, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
{
    failures.Add($"the shim was not loaded from the package's asset {packaged}");
}

// ── 2. Through the bundle's gateway ──
int port = FreePort();
using (Process gateway = await StartGateway(args[0], port))
{
    try
    {
        var uri = new Uri($"ws://127.0.0.1:{port}");
        await using var publisherClient = new FletcherClient(new FletcherClientOptions { Uri = uri });
        await using var subscriberClient = new FletcherClient(new FletcherClientOptions { Uri = uri });
        await publisherClient.ConnectAsync();
        await subscriberClient.ConnectAsync();
        FixFeed_ReportGatewayPublisher publisher = await FixFeed_ReportGatewayPublisher.CreateAsync(publisherClient);
        var received = new BlockingCollection<string>();
        await new FixFeed_ReportGatewaySubscriber(subscriberClient).SubscribeAsync((row, envelope) => received.Add(Describe(row)));
        await publisher.PublishAsync(sent);

        var viaGateway = new List<string>();
        while (viaGateway.Count < sent.Length && received.TryTake(out string? row, TimeSpan.FromSeconds(10)))
        {
            viaGateway.Add(row);
        }

        Check("gateway", viaGateway, expected, failures);
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

static void Check(string route, List<string> actual, string[] expected, List<string> failures)
{
    foreach (string line in actual)
    {
        Console.WriteLine($"{route}: {line}");
    }

    if (!actual.SequenceEqual(expected))
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
