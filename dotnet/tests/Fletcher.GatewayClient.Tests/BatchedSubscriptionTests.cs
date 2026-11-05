// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8f: FletcherClient.SubscribeBatchedAsync against the in-memory gateway (D-BIND-133).
//
// The native SubscriberArrow's rules, one case each: the three flush reasons, the deadline
// anchored to the previous flush, a dropped row counted and left out with its envelope, a
// window of only dropped rows still delivered, the byte ceiling, the Timeout bounds; then
// what the gateway client adds: one subscription's handler never concurrent with itself, the
// pending window flushed on unsubscribe, dispose and the end of the connection, a retained
// row kept until the subscription is registered, and a handler that throws.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Net.WebSockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.GatewayClient.Tests;

public sealed class BatchedSubscriptionTests
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    private static readonly GatewayBatchOptions NoDeadline = new() { MaxRows = 1000, Timeout = Timeout.InfiniteTimeSpan };

    [Fact]
    public async Task AWindowFlushesAtMaxRowsAndThePendingOneOnUnsubscribe()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var windows = new BlockingCollection<Received>();
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), Collect(windows),
            new GatewayBatchOptions { MaxRows = 3, Timeout = Timeout.InfiniteTimeSpan });

        for (int i = 1; i <= 7; ++i)
        {
            gateway.Deliver(id, Row(i));
        }

        Received first = Take(windows);
        Received second = Take(windows);
        Assert.Equal((GatewayBatchReason.RowLimit, "1,2,3", 0L), (first.Reason, first.Rows, first.Dropped));
        Assert.Equal((GatewayBatchReason.RowLimit, "4,5,6", 0L), (second.Reason, second.Rows, second.Dropped));

        await client.UnsubscribeAsync(id);
        Received last = Take(windows);
        Assert.Equal((GatewayBatchReason.Closing, "7"), (last.Reason, last.Rows));
        Assert.Empty(windows);
    }

    [Fact]
    public async Task EachRowsEnvelopeIsAtItsRowsIndexAndOutlivesTheHandler()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var kept = new BlockingCollection<(int[] Ids, IReadOnlyList<Envelope> Envelopes)>();
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), (batch, envelopes, _) =>
        {
            using (batch)
            {
                kept.Add((Ids(batch), envelopes));
            }
        }, new GatewayBatchOptions { MaxRows = 3, Timeout = Timeout.InfiniteTimeSpan });

        for (int i = 1; i <= 3; ++i)
        {
            gateway.Deliver(id, Row(i));
        }

        Assert.True(kept.TryTake(out var window, Wait));

        // Read after the handler returned and after later messages reused the receive buffer.
        gateway.Deliver(id, Row(99));
        await client.ListTopicsAsync();
        for (int i = 0; i < 3; ++i)
        {
            Assert.True(window.Envelopes[i].TryFind("n", out ReadOnlyMemory<byte> note));
            Assert.Equal(window.Ids[i].ToString(System.Globalization.CultureInfo.InvariantCulture), Encoding.UTF8.GetString(note.Span));
            Assert.Equal(EncodeRow(window.Ids[i], "r" + window.Ids[i]), window.Envelopes[i].Row.ToArray());
        }
    }

    [Fact]
    public async Task AWindowFlushesWhenItsDeadlinePasses()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var windows = new BlockingCollection<Received>();
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), Collect(windows),
            new GatewayBatchOptions { MaxRows = 1000, Timeout = TimeSpan.FromSeconds(2) });

        // Inside the first window's deadline, so both rows ride the same Timeout flush. Two seconds,
        // not 200 ms: a loaded Windows runner once let the second row arrive after a 200 ms deadline
        // (ci.pr 39592797595, BIND-9a close).
        gateway.Deliver(id, Row(1));
        gateway.Deliver(id, Row(2));

        Received window = Take(windows);
        Assert.Equal((GatewayBatchReason.Timeout, "1,2", 0L), (window.Reason, window.Rows, window.Dropped));
    }

    [Fact]
    public async Task ARowAfterAnIdleSpellLongerThanTheTimeoutIsDeliveredAtOnce()
    {
        // The deadline is anchored to the previous flush (here the subscribe), as native's.
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var windows = new BlockingCollection<Received>();
        TimeSpan timeout = TimeSpan.FromSeconds(1);
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), Collect(windows),
            new GatewayBatchOptions { MaxRows = 1000, Timeout = timeout });

        await Task.Delay(timeout + TimeSpan.FromMilliseconds(300));
        var clock = System.Diagnostics.Stopwatch.StartNew();
        gateway.Deliver(id, Row(1));

        Received window = Take(windows);
        Assert.Equal((GatewayBatchReason.Timeout, "1"), (window.Reason, window.Rows));
        Assert.True(clock.Elapsed < timeout, $"an overdue window took {clock.Elapsed}, a whole new deadline");
    }

    [Fact]
    public async Task ARowThatDoesNotDecodeIsDroppedWithItsEnvelopeCountedAndReported()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var faults = new BlockingCollection<GatewayHandlerFaultedEventArgs>();
        client.HandlerFaulted += (_, e) => faults.Add(e);
        var windows = new BlockingCollection<Received>();
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), Collect(windows),
            new GatewayBatchOptions { MaxRows = 3, Timeout = Timeout.InfiniteTimeSpan });

        gateway.Deliver(id, Row(1));
        gateway.Deliver(id, new Envelope(new byte[] { 0x00, 0x01 }, Note("bad")));
        gateway.Deliver(id, Row(3));

        Received window = Take(windows);
        Assert.Equal((GatewayBatchReason.RowLimit, "1,3", 1L), (window.Reason, window.Rows, window.Dropped));
        Assert.Equal(new[] { "1", "3" }, window.Notes);
        Assert.True(faults.TryTake(out GatewayHandlerFaultedEventArgs? fault, Wait));
        Assert.Equal((GatewayFaultReason.RowUndecodable, id), (fault!.Reason, fault.SubscriptionId));
        Assert.Equal(1ul, client.UndecodableRows);
    }

    [Fact]
    public async Task AWindowOfOnlyDroppedRowsStillDeliversAZeroRowBatch()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var windows = new BlockingCollection<Received>();
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), Collect(windows),
            new GatewayBatchOptions { MaxRows = 1000, Timeout = TimeSpan.FromMilliseconds(100) });

        // Two MESSAGEs whose envelopes do not even parse: dropped on arrival, the window empty.
        byte[] frame = new byte[8 + 3];
        System.Buffers.Binary.BinaryPrimitives.WriteUInt64LittleEndian(frame, id);
        gateway.Transport.ToClient(WebSocketMessageType.Binary, frame);
        gateway.Transport.ToClient(WebSocketMessageType.Binary, frame);

        Received window = Take(windows);
        Assert.Equal((GatewayBatchReason.Timeout, 0, 2L), (window.Reason, window.Ids.Length, window.Dropped));

        // One row whose bytes do not decode: kept until the flush, then dropped there.
        gateway.Deliver(id, new Envelope(new byte[] { 0x00, 0x01 }));

        window = Take(windows);
        Assert.Equal((GatewayBatchReason.Timeout, 0, 1L), (window.Reason, window.Ids.Length, window.Dropped));
        Assert.Empty(window.Notes);
        Assert.Equal(3ul, client.UndecodableRows);
    }

    [Fact]
    public async Task ARowThatWouldPassTheByteCeilingFlushesTheWindowFirst()
    {
        int rowBytes = EncodeRow(1, "r1").Length;
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var windows = new BlockingCollection<Received>();
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), Collect(windows),
            new GatewayBatchOptions { MaxRows = 1000, Timeout = Timeout.InfiniteTimeSpan, WindowByteCeiling = 2 * rowBytes });

        gateway.Deliver(id, Row(1));
        gateway.Deliver(id, Row(2));
        gateway.Deliver(id, Row(3));

        Received window = Take(windows);
        Assert.Equal((GatewayBatchReason.RowLimit, "1,2"), (window.Reason, window.Rows));
        await client.UnsubscribeAsync(id);
        Assert.Equal(new[] { 3 }, Take(windows).Ids);
    }

    [Fact]
    public async Task ARowLargerThanTheCeilingAloneIsDroppedAndCounted()
    {
        int rowBytes = EncodeRow(1, "r1").Length;
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var windows = new BlockingCollection<Received>();
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), Collect(windows),
            new GatewayBatchOptions { MaxRows = 1000, Timeout = Timeout.InfiniteTimeSpan, WindowByteCeiling = rowBytes });

        gateway.Deliver(id, Row(1));
        gateway.Deliver(id, new Envelope(EncodeRow(2, "a label far longer than the ceiling allows")));
        gateway.Deliver(id, Row(3));
        await client.ListTopicsAsync(); // its reply follows the rows, so the loop has read them
        await client.UnsubscribeAsync(id);

        var all = new List<Received> { Take(windows), Take(windows) };
        Assert.Equal(new[] { 1, 3 }, all.SelectMany(w => w.Ids).ToArray());
        Assert.Equal(1L, all.Sum(w => w.Dropped));
        Assert.Equal(1ul, client.UndecodableRows);
    }

    [Theory]
    [InlineData(-1.0)]
    [InlineData(50.0 * 24 * 3600)]
    public async Task ATimeoutOutsideItsBoundsIsRefused(double seconds)
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);

        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => client.SubscribeBatchedAsync("t", RowSchema(),
            (batch, _, _) => batch.Dispose(), new GatewayBatchOptions { Timeout = TimeSpan.FromSeconds(seconds) }));
        Assert.Empty(gateway.Received);
    }

    [Fact]
    public async Task TheDefaultsAreNatives()
    {
        var options = new GatewayBatchOptions();
        Assert.Equal((8000L, TimeSpan.FromMinutes(1)), (options.MaxRows, options.Timeout));
        Assert.Equal(TimeSpan.FromMilliseconds(uint.MaxValue - 1), GatewayBatchOptions.MaxTimeout);

        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), (batch, _, _) => batch.Dispose());
        Assert.Equal(1, client.SubscriptionCount);
        await client.UnsubscribeAsync(id);
    }

    [Fact]
    public async Task DisposingFlushesThePendingWindow()
    {
        var gateway = new FakeGateway();
        FletcherClient client = await Connected(gateway);
        var windows = new BlockingCollection<Received>();
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), Collect(windows), NoDeadline);
        gateway.Deliver(id, Row(1));
        gateway.Deliver(id, Row(2));
        await client.ListTopicsAsync();

        await client.DisposeAsync();

        Received window = Take(windows);
        Assert.Equal((GatewayBatchReason.Closing, "1,2"), (window.Reason, window.Rows));
    }

    [Fact]
    public async Task TheConnectionEndingFlushesThePendingWindow()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var windows = new BlockingCollection<Received>();
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), Collect(windows), NoDeadline);
        gateway.Deliver(id, Row(1));
        gateway.Close();

        Received window = Take(windows);
        Assert.Equal((GatewayBatchReason.Closing, "1"), (window.Reason, window.Rows));
        await Assert.ThrowsAsync<GatewayException>(() => client.Completion.WaitAsync(Wait));
        Assert.Equal(0, client.SubscriptionCount);
    }

    [Fact]
    public async Task ARowDeliveredBeforeTheSubscribedReplyReachesTheFirstWindow()
    {
        var gateway = new FakeGateway();
        gateway.RetainedBeforeSubscribed.Add(Row(9));
        await using FletcherClient client = await Connected(gateway);
        var windows = new BlockingCollection<Received>();

        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), Collect(windows), NoDeadline);
        await client.UnsubscribeAsync(id);

        Assert.Equal(new[] { 9 }, Take(windows).Ids);
    }

    [Fact]
    public async Task OneSubscriptionsHandlerIsNeverCalledConcurrentlyWithItself()
    {
        // Timer flushes and receive-loop flushes compete; a slow handler must still see one
        // window at a time, in order, and every row either delivered or counted.
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        int inside = 0;
        int most = 0;
        var ids = new ConcurrentQueue<int>();
        var done = new TaskCompletionSource();
        const int Rows = 40;
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), (batch, _, status) =>
        {
            using (batch)
            {
                int now = Interlocked.Increment(ref inside);
                InterlockedMax(ref most, now);
                Thread.Sleep(15);
                foreach (int i in Ids(batch))
                {
                    ids.Enqueue(i);
                }

                Interlocked.Decrement(ref inside);
                if (ids.Count == Rows)
                {
                    done.TrySetResult();
                }
            }
        }, new GatewayBatchOptions { MaxRows = 3, Timeout = TimeSpan.FromMilliseconds(5) });

        for (int i = 1; i <= Rows; ++i)
        {
            gateway.Deliver(id, Row(i));
            if (i % 4 == 0)
            {
                await Task.Delay(7);
            }
        }

        await done.Task.WaitAsync(Wait);
        Assert.Equal(1, Volatile.Read(ref most));
        Assert.Equal(Enumerable.Range(1, Rows), ids.ToArray());
    }

    [Fact]
    public async Task AWindowCutWhileTheHandlerRunsOnAnotherThreadWaitsForIt()
    {
        // Deterministic: the first window is cut by the timer, and its handler blocks there
        // while the receive loop cuts the next at MaxRows. The loop must queue it, not run the
        // handler a second time at once; the timer thread delivers it after the first returns.
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        int inside = 0;
        int most = 0;
        var entered = new ManualResetEventSlim();
        var release = new ManualResetEventSlim();
        var windows = new BlockingCollection<Received>();
        GatewayBatchHandler collect = Collect(windows);
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), (batch, envelopes, status) =>
        {
            InterlockedMax(ref most, Interlocked.Increment(ref inside));
            if (status.Reason == GatewayBatchReason.Timeout)
            {
                entered.Set();
                release.Wait(Wait);
            }

            collect(batch, envelopes, status);
            Interlocked.Decrement(ref inside);
        }, new GatewayBatchOptions { MaxRows = 2, Timeout = TimeSpan.FromMilliseconds(100) });

        gateway.Deliver(id, Row(1));
        Assert.True(entered.Wait(Wait), "the timer did not flush");
        gateway.Deliver(id, Row(2));
        gateway.Deliver(id, Row(3));
        await client.ListTopicsAsync(); // the loop has cut [2, 3] by now
        release.Set();

        Received first = Take(windows);
        Assert.Equal((GatewayBatchReason.Timeout, "1"), (first.Reason, first.Rows));
        Received second = Take(windows);
        Assert.Equal((GatewayBatchReason.RowLimit, "2,3"), (second.Reason, second.Rows));
        Assert.Equal(1, Volatile.Read(ref most));
    }

    [Fact]
    public async Task AThrowingHandlerIsCountedAndReportedAndTheSubscriptionGoesOn()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var faults = new BlockingCollection<GatewayHandlerFaultedEventArgs>();
        client.HandlerFaulted += (_, e) => faults.Add(e);
        var good = new BlockingCollection<int[]>();
        ulong id = await client.SubscribeBatchedAsync("t", RowSchema(), (batch, _, _) =>
        {
            using (batch)
            {
                int[] got = Ids(batch);
                if (got[0] == 1)
                {
                    throw new InvalidOperationException("handler bug");
                }

                good.Add(got);
            }
        }, new GatewayBatchOptions { MaxRows = 2, Timeout = Timeout.InfiniteTimeSpan });

        for (int i = 1; i <= 4; ++i)
        {
            gateway.Deliver(id, Row(i));
        }

        Assert.True(faults.TryTake(out GatewayHandlerFaultedEventArgs? fault, Wait));
        Assert.Equal((GatewayFaultReason.HandlerThrew, "handler bug"), (fault!.Reason, fault.Exception.Message));
        Assert.True(good.TryTake(out int[]? second, Wait));
        Assert.Equal(new[] { 3, 4 }, second);
        Assert.Equal(1ul, client.AbsorbedHandlerFailures);
    }

    [Fact]
    public async Task AHandlerOnTheLoopMayUnsubscribeItsOwnSubscription()
    {
        // The unsubscribe's Closing cut happens inside the handler; it must neither deadlock
        // nor run the handler re-entrantly, and rows after it are not delivered.
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var windows = new BlockingCollection<Received>();
        ulong id = 0;
        GatewayBatchHandler collect = Collect(windows);
        id = await client.SubscribeBatchedAsync("t", RowSchema(), (batch, envelopes, status) =>
        {
            _ = client.UnsubscribeAsync(id);
            collect(batch, envelopes, status);
        }, new GatewayBatchOptions { MaxRows = 2, Timeout = Timeout.InfiniteTimeSpan });

        gateway.Deliver(id, Row(1));
        gateway.Deliver(id, Row(2));
        gateway.Deliver(id, Row(3));

        Received window = Take(windows);
        Assert.Equal((GatewayBatchReason.RowLimit, "1,2"), (window.Reason, window.Rows));
        await WaitUntil(() => client.SubscriptionCount == 0);
        await client.ListTopicsAsync();
        Assert.Empty(windows);
    }

    // ── Helpers ──

    private sealed record Received(GatewayBatchReason Reason, int[] Ids, long Dropped, string[] Notes)
    {
        public string Rows => string.Join(",", Ids);
    }

    private static GatewayBatchHandler Collect(BlockingCollection<Received> into) => (batch, envelopes, status) =>
    {
        using (batch)
        {
            Assert.Equal(batch.Length, envelopes.Count);
            string[] notes = envelopes.Select(e => e.TryFind("n", out ReadOnlyMemory<byte> n) ? Encoding.UTF8.GetString(n.Span) : "").ToArray();
            into.Add(new Received(status.Reason, Ids(batch), status.RowsDropped, notes));
        }
    };

    private static Received Take(BlockingCollection<Received> from)
    {
        Assert.True(from.TryTake(out Received? received, Wait), "no window arrived");
        return received!;
    }

    private static int[] Ids(RecordBatch batch)
    {
        var column = (Int32Array)batch.Column(0);
        return Enumerable.Range(0, batch.Length).Select(i => column.GetValue(i)!.Value).ToArray();
    }

    private static void InterlockedMax(ref int target, int value)
    {
        int seen;
        while ((seen = Volatile.Read(ref target)) < value && Interlocked.CompareExchange(ref target, value, seen) != seen)
        {
        }
    }

    private static readonly Uri FakeUri = new("ws://fake:9090");

    private static async Task<FletcherClient> Connected(FakeGateway gateway)
    {
        var client = new FletcherClient(new FletcherClientOptions { Uri = FakeUri }, gateway.Transport);
        await client.ConnectAsync();
        return client;
    }

    private static Schema RowSchema() => new Schema.Builder()
        .Field(f => f.Name("id").DataType(Int32Type.Default).Nullable(false).Metadata(ControlMessages.FieldNumberKey, "1"))
        .Field(f => f.Name("label").DataType(StringType.Default).Nullable(false).Metadata(ControlMessages.FieldNumberKey, "2"))
        .Build();

    private static IEnumerable<KeyValuePair<string, ReadOnlyMemory<byte>>> Note(string note) =>
        new[] { new KeyValuePair<string, ReadOnlyMemory<byte>>("n", Encoding.UTF8.GetBytes(note)) };

    // Row i: id i, label "r<i>", attachment n = "<i>".
    private static Envelope Row(int i) =>
        new(EncodeRow(i, "r" + i), Note(i.ToString(System.Globalization.CultureInfo.InvariantCulture)));

    private static byte[] EncodeRow(int id, string label)
    {
        using var batch = new RecordBatch(RowSchema(), new IArrowArray[]
        {
            new Int32Array.Builder().Append(id).Build(),
            new StringArray.Builder().Append(label).Build(),
        }, 1);
        var codec = new GatewayRowCodec(batch.Schema);
        return codec.Encode(codec.Bind(batch), 0);
    }

    private static async Task WaitUntil(Func<bool> condition)
    {
        DateTime deadline = DateTime.UtcNow + Wait;
        while (!condition())
        {
            if (DateTime.UtcNow > deadline)
            {
                throw new TimeoutException("condition not reached");
            }

            await Task.Delay(10);
        }
    }
}
