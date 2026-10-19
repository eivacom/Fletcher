// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The C# gateway client's rows of the cross-language matrix (BIND-8, D-BIND-116): through the
// generated gateway pair, in both directions, with a C++ native peer, a C# native peer and a
// TypeScript gateway peer, over one gateway on Fast DDS. Each direction sends protoc-coverage's
// four CompositeCoverage fixture rows inside crosslang.proto's CrossRow, and the RECEIVING side
// compares every row field by field with its own transcription of the fixture: this process
// (RowCompare.cs) for rows that reach C#, each peer for rows it receives, which it reports as
// `RECV <seq> OK` or `RECV <seq> MISMATCH <diffs>`.
//
// The Fast DDS exchanges inherit gateway-fastdds-ts's exposure to the Fast DDS 3.4 data-sharing
// defect (fastdds-pubsub-provider/README.md, "Zero-copy: the plain sample"). As ruled for that
// lane by the maintainer 2026-10-16: a missing delivery names the defect and is retried ONCE,
// with a fresh client and the rows sent again; a second miss fails, naming it.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;

using Eiva.Fletcher.GatewayClient;
using Eiva.Fletcher.TestFixtures;

using Fletcher.Gen.Gateway.Crosslang;

using Xunit;
using Xunit.Abstractions;

namespace Eiva.Fletcher.CrossLang;

public sealed class CrossLangTests : IClassFixture<CrossLangLane>
{
    private const string Defect =
        "the known Fast DDS 3.4 data-sharing defect (a late-joining reader can miss TRANSIENT_LOCAL samples; " +
        "fastdds-pubsub-provider/README.md, \"Zero-copy: the plain sample\")";

    private static readonly TimeSpan Attempt = TimeSpan.FromSeconds(20);

    private readonly CrossLangLane _lane;
    private readonly ITestOutputHelper _output;

    public CrossLangTests(CrossLangLane lane, ITestOutputHelper output)
    {
        _lane = lane;
        _output = output;
    }

    // ── C# gateway client → peer: the peer compares ──────────────────────────────────────────

    [Fact]
    public Task CSharpGatewayToCpp() =>
        SendToPeerAsync(_lane.Cpp, async client => (await CrossLang_CsToCppGatewayPublisher.CreateAsync(client)).PublishAsync);

    [Fact]
    public Task CSharpGatewayToCSharpNative() =>
        SendToPeerAsync(_lane.Native, async client => (await CrossLang_CsToNativeGatewayPublisher.CreateAsync(client)).PublishAsync);

    [Fact]
    public Task CSharpGatewayToTypeScript() =>
        SendToPeerAsync(_lane.Ts, async client => (await CrossLang_CsToTsGatewayPublisher.CreateAsync(client)).PublishAsync);

    // ── peer → C# gateway client: this process compares ──────────────────────────────────────

    [Fact]
    public Task CppToCSharpGateway() =>
        ReceiveFromPeerAsync(_lane.Cpp, null, (client, handler) => new CrossLang_CppToCsGatewaySubscriber(client).SubscribeAsync((row, _) => handler(row)));

    [Fact]
    public Task CSharpNativeToCSharpGateway() =>
        ReceiveFromPeerAsync(_lane.Native, null, (client, handler) => new CrossLang_NativeToCsGatewaySubscriber(client).SubscribeAsync((row, _) => handler(row)));

    [Fact]
    public Task TypeScriptToCSharpGateway() =>
        // The TypeScript peer cannot declare a topic of CrossRow's shape (D-BIND-117), so this
        // client declares it with schemaIpc before the peer publishes.
        ReceiveFromPeerAsync(
            _lane.Ts,
            async client => await CrossLang_TsToCsGatewayPublisher.CreateAsync(client),
            (client, handler) => new CrossLang_TsToCsGatewaySubscriber(client).SubscribeAsync((row, _) => handler(row)));

    private async Task SendToPeerAsync(PeerProcess peer, Func<FletcherClient, Task<Func<IEnumerable<CrossRow>, System.Threading.CancellationToken, Task>>> publisher)
    {
        int seen = peer.Lines.Count;
        bool delivered = false;
        for (int attempt = 1; attempt <= 2 && !delivered; attempt++)
        {
            await using var client = await ConnectAsync();
            Func<IEnumerable<CrossRow>, System.Threading.CancellationToken, Task> publish = await publisher(client);
            await publish(FixtureRows(), default);
            delivered = await peer.WaitForAsync(lines => AllSeqs(lines.Skip(seen)), Attempt);
            Assert.False(peer.HasExited, $"{peer.Label} exited while receiving. Lines: {Show(peer.Lines.Skip(seen))}; stderr: {peer.StandardErrorTail}");
            if (!delivered && attempt == 1)
            {
                _output.WriteLine($"[{peer.Label}] attempt 1 missed rows within {Attempt.TotalSeconds} s: {Defect}. Retrying once. Lines: {Show(peer.Lines.Skip(seen))}");
            }
            else if (delivered && attempt == 2)
            {
                _output.WriteLine($"[{peer.Label}] the retry passed: attempt 1 hit {Defect}.");
            }
        }

        List<string> received = peer.Lines.Skip(seen).Where(l => l.StartsWith("RECV ", StringComparison.Ordinal) || l.StartsWith("ERROR ", StringComparison.Ordinal)).ToList();
        Assert.True(delivered, $"{peer.Label} did not receive rows 0 to {CoverageFixture.CompositeRowCount - 1} on either attempt ({Defect}?). Lines: {Show(received)}; stderr: {peer.StandardErrorTail}");
        List<string> wrong = received.Where(l => !l.EndsWith(" OK", StringComparison.Ordinal)).ToList();
        Assert.True(wrong.Count == 0, $"{peer.Label} received rows that differ from the fixture:\n{string.Join("\n", wrong)}");
    }

    private async Task ReceiveFromPeerAsync(
        PeerProcess peer,
        Func<FletcherClient, Task>? declare,
        Func<FletcherClient, Action<CrossRow>, Task<ulong>> subscribe)
    {
        var rows = new ConcurrentDictionary<int, List<string>>();
        var faults = new ConcurrentQueue<string>();
        bool delivered = false;
        FletcherClient? last = null;
        for (int attempt = 1; attempt <= 2 && !delivered; attempt++)
        {
            await using var client = await ConnectAsync();
            last = client;
            client.HandlerFaulted += (_, e) => faults.Enqueue(e.ToString() ?? "fault");
            if (declare is not null)
            {
                await declare(client);
            }

            try
            {
                await subscribe(client, row => rows.GetOrAdd(row.Seq, _ => new List<string>()).Add(RowCompare.ReportRow(row.Seq, row.Row)));
            }
            catch (GatewayException e)
            {
                throw new InvalidOperationException($"the gateway refused the subscription: {e.Message}; gateway stderr: {_lane.Gateway.StandardErrorTail}", e);
            }
            int seen = peer.Lines.Count;
            await peer.SendAsync("publish");
            Assert.True(
                await peer.WaitForAsync(lines => lines.Skip(seen).Any(l => l.StartsWith("PUBLISHED", StringComparison.Ordinal) || l.StartsWith("ERROR", StringComparison.Ordinal)), Attempt),
                $"{peer.Label} did not publish: {Show(peer.Lines.Skip(seen))}; stderr: {peer.StandardErrorTail}");
            Assert.DoesNotContain(peer.Lines.Skip(seen), l => l.StartsWith("ERROR", StringComparison.Ordinal));

            DateTime deadline = DateTime.UtcNow + Attempt;
            while (!Enumerable.Range(0, CoverageFixture.CompositeRowCount).All(rows.ContainsKey) && DateTime.UtcNow < deadline)
            {
                await Task.Delay(50);
            }

            delivered = Enumerable.Range(0, CoverageFixture.CompositeRowCount).All(rows.ContainsKey);
            if (!delivered && attempt == 1)
            {
                _output.WriteLine($"[{peer.Label}] attempt 1: this client received rows {string.Join(",", rows.Keys.Order())} within {Attempt.TotalSeconds} s: {Defect}. Retrying once.");
            }
            else if (delivered && attempt == 2)
            {
                _output.WriteLine($"[{peer.Label}] the retry passed: attempt 1 hit {Defect}.");
            }

            if (delivered)
            {
                Assert.True(client.UndecodableRows == 0, $"{client.UndecodableRows} rows from {peer.Label} could not be decoded");
            }
        }

        Assert.True(faults.IsEmpty, $"handler faults: {string.Join(" | ", faults)}");
        Assert.True(delivered, $"rows from {peer.Label} not received on either attempt ({Defect}?): got {string.Join(",", rows.Keys.Order())}; undecodable {last?.UndecodableRows}");
        List<string> wrong = rows.Values.SelectMany(v => v).Where(l => !l.EndsWith(" OK", StringComparison.Ordinal)).ToList();
        Assert.True(wrong.Count == 0, $"rows from {peer.Label} differ from the fixture:\n{string.Join("\n", wrong)}");
    }

    private async Task<FletcherClient> ConnectAsync()
    {
        var client = new FletcherClient(new FletcherClientOptions { Uri = _lane.Uri });
        await client.ConnectAsync();
        return client;
    }

    private static IEnumerable<CrossRow> FixtureRows() =>
        Enumerable.Range(0, CoverageFixture.CompositeRowCount).Select(seq => new CrossRow { Seq = seq, Row = CoverageFixture.CompositeRow(seq) });

    // Every fixture row has a RECV line, whatever its verdict.
    private static bool AllSeqs(IEnumerable<string> lines)
    {
        var seqs = lines.Where(l => l.StartsWith("RECV ", StringComparison.Ordinal))
            .Select(l => l.Split(' ')[1])
            .ToHashSet();
        return Enumerable.Range(0, CoverageFixture.CompositeRowCount).All(s => seqs.Contains($"{s}"));
    }

    private static string Show(IEnumerable<string> lines) => "[" + string.Join(" | ", lines) + "]";
}
