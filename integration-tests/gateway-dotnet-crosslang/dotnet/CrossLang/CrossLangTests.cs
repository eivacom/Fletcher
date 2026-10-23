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
// The fixture rows travel LIVE (D-BIND-129): before them the publisher sends probe rows (seq
// -1, never compared) until one arrives, so the reader has matched the writer and nothing
// depends on a Fast DDS backlog replay, the path the Fast DDS 3.4 data-sharing defect breaks
// (fastdds-pubsub-provider/README.md, "Zero-copy: the plain sample"). As ruled for that defect
// by the maintainer 2026-10-16, a missed delivery still names it and is retried ONCE, with a
// fresh client; each attempt must deliver all four rows by itself (review B3).
//
// The C# NATIVE pair's rows (BIND-9b-4, D-BIND-161) run here too, peer to peer: this process
// only drives them. One peer publishes on its command, another receives and compares, so these
// rows cross no C# gateway client. A peer listens on more than one topic, so each of its lines
// names the topic (`<topic> RECV <seq> ...`) and each command names one (`publish <topic>`): a
// late row of an earlier test never counts for the next.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
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
        SendToPeerAsync(_lane.Cpp, "CsToCpp", async client => (await CrossLang_CsToCppGatewayPublisher.CreateAsync(client)).PublishAsync);

    [Fact]
    public Task CSharpGatewayToCSharpNative() =>
        SendToPeerAsync(_lane.Native, "CsToNative", async client => (await CrossLang_CsToNativeGatewayPublisher.CreateAsync(client)).PublishAsync);

    [Fact]
    public Task CSharpGatewayToTypeScript() =>
        SendToPeerAsync(_lane.Ts, "CsToTs", async client => (await CrossLang_CsToTsGatewayPublisher.CreateAsync(client)).PublishAsync);

    // ── peer → C# gateway client: this process compares ──────────────────────────────────────
    // The TypeScript peer declares its own topic since D-BIND-130 (its generated schema carries
    // the Arrow IPC bytes), as the native peers do.

    [Fact]
    public Task CppToCSharpGateway() =>
        ReceiveFromPeerAsync(_lane.Cpp, "CppToCs", (client, handler) => new CrossLang_CppToCsGatewaySubscriber(client).SubscribeAsync((row, _) => handler(row)));

    [Fact]
    public Task CSharpNativeToCSharpGateway() =>
        ReceiveFromPeerAsync(_lane.Native, "NativeToCs", (client, handler) => new CrossLang_NativeToCsGatewaySubscriber(client).SubscribeAsync((row, _) => handler(row)));

    [Fact]
    public Task TypeScriptToCSharpGateway() =>
        ReceiveFromPeerAsync(_lane.Ts, "TsToCs", (client, handler) => new CrossLang_TsToCsGatewaySubscriber(client).SubscribeAsync((row, _) => handler(row)));

    // ── C# native pair ↔ peer (BIND-9b-4, D-BIND-161): the receiving peer compares ───────────

    [Fact]
    public Task CSharpNativeToCpp() => PeerToPeerAsync(_lane.Native, _lane.Cpp, "NativeToCpp");

    [Fact]
    public Task CppToCSharpNative() => PeerToPeerAsync(_lane.Cpp, _lane.Native, "CppToNative");

    [Fact]
    public Task CSharpNativeToTypeScript() => PeerToPeerAsync(_lane.Native, _lane.Ts, "NativeToTs");

    [Fact]
    public Task TypeScriptToCSharpNative() => PeerToPeerAsync(_lane.Ts, _lane.Native, "TsToNative");

    [Fact]
    public Task CSharpNativeToCSharpNative() => PeerToPeerAsync(_lane.Native, _lane.Native2, "NativeToNative");

    // A fresh C# gateway client per attempt publishes; the peer compares.
    private Task SendToPeerAsync(PeerProcess peer, string topic, Func<FletcherClient, Task<Func<IEnumerable<CrossRow>, CancellationToken, Task>>> publisher) =>
        DeliverToPeerAsync(peer, topic, async () =>
        {
            FletcherClient client = await ConnectAsync();
            Func<IEnumerable<CrossRow>, CancellationToken, Task> publish = await publisher(client);
            return new Sender(() => publish(new[] { ProbeRow() }, default), () => publish(FixtureRows(), default), client, null);
        });

    // One peer publishes on its commands; the other compares.
    private Task PeerToPeerAsync(PeerProcess publisher, PeerProcess receiver, string topic) =>
        DeliverToPeerAsync(receiver, topic, () => Task.FromResult(new Sender(
            () => SendAndAwaitAsync(publisher, $"probe {topic}", "PROBED"),
            () => SendAndAwaitAsync(publisher, $"publish {topic}", "PUBLISHED"),
            null,
            publisher)));

    // How one attempt sends: a probe row, the fixture rows, what to dispose after the attempt,
    // and the publishing peer, if a peer publishes (its STATUS lines join a failure's message).
    private sealed record Sender(Func<Task> Probe, Func<Task> Publish, IAsyncDisposable? Owner, PeerProcess? Publisher);

    // One attempt delivers all four rows on its own, or it is retried once, with the sender made
    // again and the rows sent again (review B3: rows of two partial attempts never add up to one).
    private async Task DeliverToPeerAsync(PeerProcess peer, string topic, Func<Task<Sender>> makeSender)
    {
        List<string> received = new();
        bool delivered = false;
        PeerProcess? publisher = null;
        for (int attempt = 1; attempt <= 2 && !delivered; attempt++)
        {
            Sender sender = await makeSender();
            publisher = sender.Publisher;
            try
            {
                // D-BIND-129: probe rows until one arrives, so the fixture rows travel live, not
                // through the backlog a late match replays.
                int probed = peer.Lines.Count;
                bool matched = false;
                DateTime deadline = DateTime.UtcNow + Attempt;
                while (!matched && DateTime.UtcNow < deadline)
                {
                    await sender.Probe();
                    matched = await peer.WaitForAsync(lines => lines.Skip(probed).Any(l => IsProbe(l, topic)), ProbeWait);
                    Assert.False(peer.HasExited, $"{peer.Label} exited while probing; stderr: {peer.StandardErrorTail}");
                }

                int seen = peer.Lines.Count;
                if (matched)
                {
                    await sender.Publish();
                    delivered = await peer.WaitForAsync(lines => AllSeqs(lines.Skip(seen), topic), Attempt);
                }

                Assert.False(peer.HasExited, $"{peer.Label} exited while receiving. Lines: {Show(peer.Lines.Skip(seen))}; stderr: {peer.StandardErrorTail}");
                received = peer.Lines.Skip(seen).Where(l => (IsRecv(l, topic) && !IsProbe(l, topic)) || l.StartsWith("ERROR ", StringComparison.Ordinal)).ToList();
                Note(peer, attempt, delivered, matched, Show(received));
            }
            finally
            {
                if (sender.Owner is not null)
                {
                    await sender.Owner.DisposeAsync();
                }
            }
        }

        Assert.True(delivered, $"{peer.Label} did not receive rows 0 to {CoverageFixture.CompositeRowCount - 1} on {topic} in one attempt, twice ({Defect}?). Lines: {Show(received)}; stderr: {peer.StandardErrorTail}; {Statuses(topic, peer, publisher)}");
        List<string> wrong = received.Where(l => !l.EndsWith(" OK", StringComparison.Ordinal)).ToList();
        Assert.True(wrong.Count == 0, $"{peer.Label} received rows on {topic} that differ from the fixture:\n{string.Join("\n", wrong)}");
    }

    private async Task ReceiveFromPeerAsync(PeerProcess peer, string topic, Func<FletcherClient, Action<CrossRow>, Task<ulong>> subscribe)
    {
        var faults = new ConcurrentQueue<string>();
        ConcurrentDictionary<int, List<string>> rows = new();
        bool delivered = false;
        ulong undecodable = 0;
        for (int attempt = 1; attempt <= 2 && !delivered; attempt++)
        {
            var attemptRows = new ConcurrentDictionary<int, List<string>>();
            int probesSeen = 0;
            await using var client = await ConnectAsync();
            client.HandlerFaulted += (_, e) => faults.Enqueue($"{e.Reason}: {e.Exception}");
            try
            {
                await subscribe(client, row =>
                {
                    if (row.Seq < 0)
                    {
                        Interlocked.Increment(ref probesSeen);
                        return;
                    }

                    attemptRows.GetOrAdd(row.Seq, _ => new List<string>()).Add(RowCompare.ReportRow(row.Seq, row.Row));
                });
            }
            catch (GatewayException e)
            {
                throw new InvalidOperationException($"the gateway refused the subscription: {e.Message}; gateway stderr: {_lane.Gateway.StandardErrorTail}", e);
            }

            // D-BIND-129: the peer publishes probe rows until one arrives through the gateway, so
            // the gateway's reader has matched the peer's writer before the fixture rows go.
            DateTime deadline = DateTime.UtcNow + Attempt;
            while (Volatile.Read(ref probesSeen) == 0 && DateTime.UtcNow < deadline)
            {
                await SendAndAwaitAsync(peer, $"probe {topic}", "PROBED");
                DateTime probeEnd = DateTime.UtcNow + ProbeWait;
                while (Volatile.Read(ref probesSeen) == 0 && DateTime.UtcNow < probeEnd)
                {
                    await Task.Delay(20);
                }
            }

            bool matched = Volatile.Read(ref probesSeen) > 0;
            if (matched)
            {
                await SendAndAwaitAsync(peer, $"publish {topic}", "PUBLISHED");
                deadline = DateTime.UtcNow + Attempt;
                while (!Enumerable.Range(0, CoverageFixture.CompositeRowCount).All(attemptRows.ContainsKey) && DateTime.UtcNow < deadline)
                {
                    await Task.Delay(50);
                }
            }

            delivered = matched && Enumerable.Range(0, CoverageFixture.CompositeRowCount).All(attemptRows.ContainsKey);
            rows = attemptRows;
            undecodable = client.UndecodableRows;
            Note(peer, attempt, delivered, matched, "rows " + string.Join(",", attemptRows.Keys.Order()));
        }

        Assert.True(faults.IsEmpty, $"handler faults: {string.Join(" | ", faults)}");
        Assert.True(undecodable == 0, $"{undecodable} rows from {peer.Label} could not be decoded");
        Assert.True(delivered, $"rows from {peer.Label} not all received in one attempt, twice ({Defect}?): got {string.Join(",", rows.Keys.Order())}");
        List<string> wrong = rows.Values.SelectMany(v => v).Where(l => !l.EndsWith(" OK", StringComparison.Ordinal)).ToList();
        Assert.True(wrong.Count == 0, $"rows from {peer.Label} differ from the fixture:\n{string.Join("\n", wrong)}");
    }

    private static readonly TimeSpan ProbeWait = TimeSpan.FromSeconds(1);

    // Sends a peer command and waits for its acknowledgement line; an ERROR line fails at once.
    private static async Task SendAndAwaitAsync(PeerProcess peer, string command, string done)
    {
        int seen = peer.Lines.Count;
        await peer.SendAsync(command);
        Assert.True(
            await peer.WaitForAsync(lines => lines.Skip(seen).Any(l => l.StartsWith(done, StringComparison.Ordinal) || l.StartsWith("ERROR", StringComparison.Ordinal)), TimeSpan.FromSeconds(10)),
            $"{peer.Label} did not answer '{command}': {Show(peer.Lines.Skip(seen))}; stderr: {peer.StandardErrorTail}");
        Assert.DoesNotContain(peer.Lines.Skip(seen), l => l.StartsWith("ERROR", StringComparison.Ordinal));
    }

    private void Note(PeerProcess peer, int attempt, bool delivered, bool matched, string what)
    {
        if (!delivered && attempt == 1)
        {
            _output.WriteLine($"[{peer.Label}] attempt 1 did not deliver every row ({(matched ? "matched" : "no probe arrived")}; {what}) within {Attempt.TotalSeconds} s: {Defect}. Retrying once.");
        }
        else if (delivered && attempt == 2)
        {
            _output.WriteLine($"[{peer.Label}] the retry passed: attempt 1 hit {Defect}.");
        }
    }

    private async Task<FletcherClient> ConnectAsync()
    {
        var client = new FletcherClient(new FletcherClientOptions { Uri = _lane.Uri });
        await client.ConnectAsync();
        return client;
    }

    private static IEnumerable<CrossRow> FixtureRows() =>
        Enumerable.Range(0, CoverageFixture.CompositeRowCount).Select(seq => new CrossRow { Seq = seq, Row = CoverageFixture.CompositeRow(seq) });

    // A probe row: seq -1, which no receiver compares (each reports it as UNKNOWN).
    private static CrossRow ProbeRow() => new() { Seq = -1 };

    // The STATUS lines a peer printed for the topic (the C++ peer prints Fast DDS's match and loss
    // statuses), to tell a lost schema from a lost row or a missing match when a delivery fails.
    private static string Statuses(string topic, params PeerProcess?[] peers) =>
        string.Join("; ", peers.Where(p => p is not null).Distinct().Select(p =>
            $"{p!.Label} statuses: {Show(p.Lines.Where(l => l.StartsWith("STATUS ", StringComparison.Ordinal) && l.Contains($"/{topic} ", StringComparison.Ordinal)))}"));

    private static bool IsRecv(string line, string topic) => line.StartsWith($"{topic} RECV ", StringComparison.Ordinal);

    private static bool IsProbe(string line, string topic) => line.StartsWith($"{topic} RECV -1 ", StringComparison.Ordinal);

    // Every fixture row has a RECV line on the topic, whatever its verdict.
    private static bool AllSeqs(IEnumerable<string> lines, string topic)
    {
        var seqs = lines.Where(l => IsRecv(l, topic))
            .Select(l => l.Split(' ')[2])
            .ToHashSet();
        return Enumerable.Range(0, CoverageFixture.CompositeRowCount).All(s => seqs.Contains($"{s}"));
    }

    private static string Show(IEnumerable<string> lines) => "[" + string.Join(" | ", lines) + "]";
}
