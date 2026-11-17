// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D10C-10, rung 11: a window the server shim cannot hand over reaches the C# handler.
//
// ── What is proven, and through what ────────────────────────────────────────
// Ruling :551 (3): "A failed hand-over reaches a C# handler with a null batch, its rows
// counted, and the failure's status, origin `Handover` (4) and message" (d10 L9, L14; d9 L18,
// D30, D32; register BC-12, C# half). This assembly loads d9's test-only hand-over fault shim,
// staged under the server file name (see the csproj), so everything between the test and the
// shim is the code that ships: `SubscriberArrow.SubscribeBatched`, the batch thunk, domain 7's
// batcher and the shim's hand-over. `fl_test_handover_fault_arm(n)` makes the n-th hand-over
// from then on throw `std::bad_alloc` after the export, once; `fl_test_handover_fault_fired()`
// says how many armed faults fired since the last arm, so a run in which nothing failed cannot
// pass for a green one.
//
// ── The message ─────────────────────────────────────────────────────────────
// Asserted present, and not the binding's no-message fallback (Errors.NoMessage), never word
// for word: it is `std::bad_alloc().what()`, which differs by C++ runtime (step-0a audit (c)).
// Byte-exactness is the internal leg's (Fletcher.Tests,
// SubscriberArrowTests.TheBatchThunkForwardsAFailureAsGiven), which drives the same thunk.
//
// ── One process, one counter ────────────────────────────────────────────────
// The fault counter is process-wide, so the tests here run one at a time (design P4).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;

using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace Eiva.Fletcher.Handover.Tests;

public sealed partial class HandoverTests : IDisposable
{
    private static readonly Schema TwoColumns = new(
    [
        new Field("x", Int32Type.Default, nullable: true),
        new Field("name", StringType.Default, nullable: true),
    ], metadata: null);

    /// <summary>
    /// The binding's text for a failure that carries no message (<c>Errors.NoMessage</c>). A message
    /// that never crossed arrives as this, so it must not pass for the shim's. Read from the binding,
    /// not copied, so the check cannot go vacuous if the text changes (step-0b audit note 1).
    /// </summary>
    private static string NoMessageFallback =>
        typeof(SubscriberArrow).Assembly.GetType("Eiva.Fletcher.Errors")?
            .GetField("NoMessage", BindingFlags.NonPublic | BindingFlags.Static)?.GetRawConstantValue() as string
        ?? throw new InvalidOperationException("Eiva.Fletcher.Errors.NoMessage is gone; re-point this test at the fallback text");

    /// <summary>The server build's file name on this platform: where the fault shim is staged.</summary>
    private static string StagedFileName =>
        OperatingSystem.IsWindows() ? "fletcher-c-abi-server.dll"
        : OperatingSystem.IsMacOS() ? "libfletcher-c-abi-server.dylib"
        : "libfletcher-c-abi-server.so";

    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly SubscriberArrow _subscriber;
    private readonly Sink _sink = new();

    public HandoverTests()
    {
        // Fail, never skip, when the fault shim was not staged: name the property that stages it.
        Assert.True(
            File.Exists(Path.Combine(AppContext.BaseDirectory, StagedFileName)),
            $"the hand-over fault shim is not staged beside the tests as '{StagedFileName}': build with " +
            "-p:FletcherHandoverFaultShim=<path to fletcher-c-abi-handover-fault>, from c-abi's test build of " +
            "the server variant (dotnet/README.md).");

        _provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        _publisher = new Publisher(_provider);
        _subscriber = new SubscriberArrow(_provider);
    }

    public void Dispose()
    {
        FaultShim.fl_test_handover_fault_arm(0);  // disarm: a cell that failed early leaves no fault armed (4b N3)
        _subscriber.Dispose();
        _publisher.Dispose();
        _provider.Dispose();
        _sink.Dispose();
    }

    /// <summary>
    /// The forcing test (rung 11; ruling :551 (3), :512, :383 (1); d10 L9, L14). One batched
    /// subscription, windows of three rows and no deadline; the fault is armed after subscribing,
    /// then the rows go out and the subscription is closed from outside, which returns after the
    /// Closing, so every delivery is in when it returns and no timing is needed.
    /// <list type="bullet">
    /// <item>(A) <c>arm(2)</c>, 9 rows: window 2 (row limit) fails; windows 1 and 3 arrive, then the idle Closing.</item>
    /// <item>(B) <c>arm(3)</c>, 7 rows: the Closing window (1 row) fails, and the close still returns.</item>
    /// </list>
    /// The failed window arrives once, as a null batch with its rows counted and its reason kept,
    /// carrying the shim's failure: Internal, origin Handover, and a message. Every other window
    /// carries none, and no row goes uncounted.
    /// </summary>
    [Theory]
    [InlineData(2, 9, BatchReason.RowLimit, 3)]
    [InlineData(3, 7, BatchReason.Closing, 1)]
    public void AWindowTheShimCannotHandOverReachesTheHandlerWithItsFailure(
        long nth, int rows, BatchReason failedReason, long failedRows)
    {
        TopicPath topic = Declared(nth == 2 ? "rowlimit" : "closing");
        SubscribeResult result = _subscriber.SubscribeBatched(
            topic, _sink.Handler, new BatchOptions { MaxRows = 3, Timeout = Timeout.InfiniteTimeSpan });
        result.Schema.Dispose();

        FaultShim.fl_test_handover_fault_arm(nth);  // counted from here, so armed before the rows go out
        PublishRows(topic, rows);
        _subscriber.Unsubscribe(result.Subscription);  // from outside: every delivery is in when it returns
        List<Delivery> seen = _sink.Snapshot();

        Assert.True(FaultShim.fl_test_handover_fault_fired() == 1,
            "the armed hand-over did not fail, so this run proves nothing");
        AssertEveryRowCountedAndOneClosingLast(seen, rows);

        Delivery failed = Assert.Single(seen, d => d.Status.Failure is not null);
        Assert.Equal((int)nth - 1, seen.IndexOf(failed));
        Assert.Null(failed.Batch);
        Assert.Empty(failed.Attachments);
        Assert.Equal(failedReason, failed.Status.Reason);
        Assert.Equal(failedRows, failed.Status.RowsDropped);

        FletcherException failure = failed.Status.Failure!;
        Assert.Equal(FletcherStatus.Internal, failure.Status);
        Assert.Equal(FletcherOrigin.Handover, failure.Origin);
        Assert.False(string.IsNullOrEmpty(failure.Message), "the hand-over failure arrived with no message");
        Assert.NotEqual(NoMessageFallback, failure.Message);

        Assert.Equal(0UL, _subscriber.AbsorbedCallbackFailures);
    }

    /// <summary>
    /// The control (a guard, green before and after rung 11): the forcing test's publish and close
    /// with nothing armed. No hand-over fails, so no window carries a failure: C# invents none.
    /// <c>arm(0)</c> disarms and zeroes the fired count (handover_fault.cpp), so an earlier test in
    /// this process cannot leak into this one.
    /// </summary>
    [Theory]
    [InlineData(9)]
    [InlineData(7)]
    public void UnarmedNoWindowCarriesAFailure(int rows)
    {
        TopicPath topic = Declared("unarmed" + rows);
        SubscribeResult result = _subscriber.SubscribeBatched(
            topic, _sink.Handler, new BatchOptions { MaxRows = 3, Timeout = Timeout.InfiniteTimeSpan });
        result.Schema.Dispose();

        FaultShim.fl_test_handover_fault_arm(0);
        PublishRows(topic, rows);
        _subscriber.Unsubscribe(result.Subscription);
        List<Delivery> seen = _sink.Snapshot();

        Assert.Equal(0, FaultShim.fl_test_handover_fault_fired());
        AssertEveryRowCountedAndOneClosingLast(seen, rows);
        Assert.All(seen, d => Assert.Null(d.Status.Failure));
        Assert.Equal(0L, seen.Sum(d => d.Status.RowsDropped));
    }

    // ── Helpers ─────────────────────────────────────────────────────────────

    /// <summary>
    /// Rows offered = rows delivered + rows counted lost (ruling :383 (1)), and exactly one Closing,
    /// the last delivery (ruling :484; d7 L6, L9).
    /// </summary>
    private static void AssertEveryRowCountedAndOneClosingLast(List<Delivery> seen, int rows)
    {
        long received = seen.Sum(d => (long)(d.Batch?.Length ?? 0));
        long lost = seen.Sum(d => d.Status.RowsDropped);
        Assert.True(received + lost == rows,
            $"rows received ({received}) + rows lost ({lost}) != rows published ({rows}): a window vanished uncounted");
        Assert.Single(seen, d => d.Status.Reason == BatchReason.Closing);
        Assert.Equal(BatchReason.Closing, seen[^1].Status.Reason);
    }

    private TopicPath Declared(string name)
    {
        TopicPath topic = TopicPath.Of("d10c10", "handover", name);
        _publisher.CreateTopic(topic, TwoColumns);
        return topic;
    }

    private void PublishRows(TopicPath topic, int rows)
    {
        using var codec = new FletcherCodec(TwoColumns);
        for (int i = 0; i < rows; i++)
        {
            using RecordBatch batch = new(
                TwoColumns,
                [new Int32Array.Builder().Append(i).Build(), new StringArray.Builder().Append("row" + i).Build()],
                length: 1);
            using BoundRows bound = codec.Bind(batch);
            _publisher.Publish(topic, bound, 0);
        }
    }

    private sealed record Delivery(RecordBatch? Batch, IReadOnlyList<AttachmentsBuilder> Attachments, BatchStatus Status);

    /// <summary>Collects deliveries and disposes what it was handed, which it owns.</summary>
    private sealed class Sink : IDisposable
    {
        private readonly object _gate = new();
        private readonly List<Delivery> _deliveries = [];

        internal RecordBatchHandler Handler => (batch, attachments, status) =>
        {
            lock (_gate)
            {
                _deliveries.Add(new Delivery(batch, attachments, status));
            }
        };

        internal List<Delivery> Snapshot()
        {
            lock (_gate)
            {
                return [.. _deliveries];
            }
        }

        public void Dispose()
        {
            lock (_gate)
            {
                foreach (Delivery d in _deliveries)
                {
                    d.Batch?.Dispose();
                    foreach (AttachmentsBuilder a in d.Attachments)
                    {
                        a.Dispose();
                    }
                }
            }
        }
    }

    /// <summary>
    /// The fault shim's two test-only exports (c-abi/tests/handover_fault.cpp), declared HERE and
    /// nowhere in <c>Eiva.Fletcher.Interop</c>: the shipped shims do not export them. Imported by
    /// the staged server file name, which resolves to the module the binding's resolver already
    /// loaded (same file, same folder), so the process holds one shim (d9 L17; design P3).
    /// </summary>
    private static partial class FaultShim
    {
        [LibraryImport("fletcher-c-abi-server")]
        internal static partial void fl_test_handover_fault_arm(long nth);

        [LibraryImport("fletcher-c-abi-server")]
        internal static partial long fl_test_handover_fault_fired();
    }
}
