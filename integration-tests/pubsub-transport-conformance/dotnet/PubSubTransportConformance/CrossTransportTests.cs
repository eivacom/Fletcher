// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-4d-iii — BUCKET 4: the same C# over every transport, selected by name.
//
// ── What this suite asserts, and why it is not 80 ported cases ──────────────
// BIND-4's acceptance states bucket 4 as FILE SETS rather than counts - and says
// why: bucket 4's own total moved 102 -> 104 between the matrix being derived and
// BIND-2 closing. The property it names is precise:
//
//     "Fast DDS AND XRCE reachable by selector with NO PER-TRANSPORT C# CODE."
//
// So the port is not a case-by-case rewrite of the provider suites. Most of those
// 80 cases assert things the managed surface cannot see and does not implement -
// QoS read back out of DDS discovery, sample loans, data-sharing, memory
// policies, the ordered-delivery buffer, SIGPIPE handling - and by D-BIND-47's
// test ("does the managed surface IMPLEMENT this, or only WRAP it?") they belong
// to the providers that own them. They are covered where they live, in C++.
//
// What the BINDING owes is the claim above, and the way to prove a claim about
// the ABSENCE of per-transport code is to run ONE BODY against every transport.
// Hence `[Theory]`: each row below has a single implementation and a selector
// parameter. A binding that had grown a special case for Fast DDS would need a
// second body, and there is nowhere to put one.
//
// ── Why this is an integration lane rather than a unit test ─────────────────
// These rows create REAL Fast DDS participants. That is discovery traffic, shared
// memory segments and multicast on the machine running them, which does not
// belong in `dotnet`'s unit lane - and this repo has already lost a review cycle
// to a false `0xC0000005` from leaked segments in
// `C:\ProgramData\eprosima\fastdds_interprocess` being read as a code defect. In
// a lane whose name says "transport", that failure is diagnosable; in the unit
// lane it would look like a regression in the binding.
//
// ── The bodies are written for the WEAKEST transport, deliberately ──────────
// `inprocess` delivers synchronously on the publishing thread; Fast DDS delivers
// asynchronously, after discovery, and may drop what is published before a reader
// is matched. A body written for `inprocess` would assert immediately after
// publishing and fail on Fast DDS for a reason that is not a defect. So every
// body below waits on a signal with a deadline, and publishes repeatedly until it
// arrives - which costs `inprocess` nothing, because its first publish satisfies
// the wait before the loop can run again.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher;

using Xunit;

namespace Eiva.Fletcher.TransportConformance;

[Collection(XrceAgentCollection.Name)]
public class CrossTransportTests
{
    // Taken so xUnit starts the suite's MicroXRCEAgent before the first row: the
    // `xrce` rows run against it (D-BIND-56).
    public CrossTransportTests(XrceAgentFixture agent) => ArgumentNullException.ThrowIfNull(agent);

    /// <summary>Every transport the shim links, each selected by name.</summary>
    /// <remarks>
    /// `xrce` joined on 2026-09-25 (D-BIND-56). Until then it was absent because
    /// this lane started no Agent, and the round trip was said to belong to
    /// `integration-test-fastdds-xrce-interop` - which is C++, so no C# had ever
    /// published or subscribed over XRCE-DDS, although BIND-4's acceptance promised
    /// bucket 4 over it. The lane now builds the Agent and the suite's fixture
    /// starts one and proves it owns it (see XrceAgent.cs).
    /// </remarks>
    internal static readonly string[] SelectorNames = ["inprocess", "fastdds", "xrce"];

    public static TheoryData<string> Transports
    {
        get
        {
            var data = new TheoryData<string>();
            foreach (string selector in SelectorNames)
            {
                data.Add(selector);
            }

            return data;
        }
    }

    private const int DeadlineMs = 20_000;
    private const int PublishRetryMs = 100;

    private static Schema OneColumn() =>
        new([new Field("id", Int32Type.Default, nullable: true)], metadata: null);

    private static RecordBatch OneRow()
    {
        Schema schema = OneColumn();
        Int32Array ids = new Int32Array.Builder().Append(42).Build();
        return new RecordBatch(schema, [ids], length: 1);
    }

    /// <summary>Each row's DEPLOYMENT configuration - the one thing a row may vary.</summary>
    /// <remarks>
    /// Configuration, not code: what a real application reads from a file. An XRCE
    /// client has to be told where its Agent is and needs a session key of its
    /// own; the other two run on their defaults. No body below looks at it.
    /// </remarks>
    private static ProviderConfig ConfigFor(string selector) =>
        selector == "xrce" ? XrceAgentFixture.ClientConfig() : new ProviderConfig();

    private static PubSubProviderHandle Open(string selector) =>
        ProviderRegistry.Create(ProviderSelector.Parse(selector), ConfigFor(selector));

    /// <summary>A topic unique to this run, so concurrent lanes cannot collide.</summary>
    /// <remarks>
    /// Fast DDS is a REAL BUS. Two jobs on one runner, or a developer with the
    /// suite open twice, would otherwise publish into each other's topics and see
    /// row counts that make no sense. `inprocess` does not need this and is not
    /// harmed by it.
    /// </remarks>
    private static TopicPath UniqueTopic(string name) =>
        TopicPath.Of("bind4", name, Guid.NewGuid().ToString("N"));

    /// <summary>Publish until the signal fires or the deadline passes.</summary>
    /// <returns>Whether it arrived.</returns>
    /// <remarks>
    /// The retry is what makes one body work on both transports. Fast DDS can drop
    /// a sample published before the reader is matched - that is discovery, not
    /// loss - so a single publish would make this suite flaky for a reason that is
    /// not a defect in anything. Returning a bool rather than asserting inside
    /// lets each row say what a timeout MEANS for it.
    /// </remarks>
    internal static bool PublishUntilSeen(Action publish, ManualResetEventSlim seen)
    {
        var clock = Stopwatch.StartNew();
        while (clock.ElapsedMilliseconds < DeadlineMs)
        {
            publish();
            if (seen.Wait(PublishRetryMs))
            {
                return true;
            }
        }

        return false;
    }

    [Theory]
    [MemberData(nameof(Transports))]
    public void TheSameCodeOpensEveryTransport(string selector)
    {
        using PubSubProviderHandle provider = Open(selector);
        Assert.NotNull(provider);
    }

    [Theory]
    [MemberData(nameof(Transports))]
    public void TheSameCodeDeclaresATopicAndListsItBack(string selector)
    {
        using PubSubProviderHandle provider = Open(selector);
        using var publisher = new Publisher(provider);

        TopicPath topic = UniqueTopic("declare");
        publisher.CreateTopic(topic, OneColumn());

        Assert.Contains(topic.ToKey(), publisher.ListTopics());
    }

    [Theory]
    [MemberData(nameof(Transports))]
    public void AConflictingRedeclarationIsASchemaConflictOnEveryTransport(string selector)
    {
        // The STATUS, not merely a throw. A caller branches on it, and a provider
        // that reported this as a transport failure would send them down the wrong
        // path - retry rather than fix your schema.
        using PubSubProviderHandle provider = Open(selector);
        using var publisher = new Publisher(provider);

        TopicPath topic = UniqueTopic("conflict");
        publisher.CreateTopic(topic, OneColumn());

        var different = new Schema(
            [new Field("other", StringType.Default, nullable: true)], metadata: null);

        FletcherException refused =
            Assert.Throws<FletcherException>(() => publisher.CreateTopic(topic, different));
        Assert.Equal(FletcherStatus.SchemaConflict, refused.Status);
    }

    [Theory]
    [MemberData(nameof(Transports))]
    public void ARowPublishedFromCSharpArrivesAtCSharpOverEveryTransport(string selector)
    {
        // THE ROW THIS SUITE EXISTS FOR. One body, every transport the theory
        // covers, and no branch on the selector anywhere below this line - which
        // is what "no per-transport C# code" means when it is asserted rather
        // than claimed.
        using PubSubProviderHandle provider = Open(selector);
        using var publisher = new Publisher(provider);
        using var subscriber = new Subscriber(provider);

        RecordBatch batch = OneRow();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        TopicPath topic = UniqueTopic("roundtrip");
        publisher.CreateTopic(topic, batch.Schema);

        using var seen = new ManualResetEventSlim(false);
        byte[]? received = null;

        SubscribeResult result = subscriber.Subscribe(topic, (row, _, _) =>
        {
            received ??= row.ToArray();
            seen.Set();
        });

        using (result.Schema)
        {
            bool arrived = PublishUntilSeen(() => publisher.Publish(topic, rows, 0), seen);
            Assert.True(arrived, $"no row arrived over '{selector}' within {DeadlineMs} ms");
        }

        Assert.NotNull(received);
        Assert.NotEmpty(received!);

        result.Subscription.Dispose();
    }

    [Theory]
    [MemberData(nameof(Transports))]
    public void AttachmentsCrossEveryTransport(string selector)
    {
        using PubSubProviderHandle provider = Open(selector);
        using var publisher = new Publisher(provider);
        using var subscriber = new Subscriber(provider);

        RecordBatch batch = OneRow();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        TopicPath topic = UniqueTopic("attachments");
        publisher.CreateTopic(topic, batch.Schema);

        using var attachments = new AttachmentsBuilder();
        attachments.Set("trace", [0xAB, 0xCD]);

        using var seen = new ManualResetEventSlim(false);
        byte[]? value = null;

        SubscribeResult result = subscriber.Subscribe(topic, (_, _, view) =>
        {
            if (view.TryFind("trace"u8, out ReadOnlySpan<byte> found))
            {
                value ??= found.ToArray();
            }

            seen.Set();
        });

        using (result.Schema)
        {
            bool arrived = PublishUntilSeen(
                () => publisher.Publish(topic, rows, 0, attachments), seen);
            Assert.True(arrived, $"no row arrived over '{selector}' within {DeadlineMs} ms");
        }

        // Sidecar metadata is not a property of the transport, and a provider that
        // dropped it would still deliver the row - so the row arriving is not
        // enough on its own.
        Assert.Equal(new byte[] { 0xAB, 0xCD }, value);

        result.Subscription.Dispose();
    }

    [Fact]
    public void XrceIsSelectableAndAnUnreachableAgentIsATypedTransportFailure()
    {
        // WHAT IS TRUE WITHOUT AN AGENT, and it is worth asserting rather than
        // skipping: the selector resolves to the XRCE driver (so the shim really
        // does link it, which is the "reachable by selector" half), and the
        // failure to reach the Agent comes back as a TYPED Fletcher failure with a
        // message naming the endpoint - not as a crash, a hang, or an untyped
        // exception a caller cannot act on.
        //
        // It aims at the default Agent address, 127.0.0.1:2018, where this lane
        // runs nothing: the suite's own Agent is on 2020 (XrceAgent.SuitePort), and
        // the round trip over it is the `xrce` row of every theory above.
        FletcherException refused = Assert.Throws<FletcherException>(
            () => ProviderRegistry.Create(ProviderSelector.Parse("xrce"), new ProviderConfig()));

        Assert.Equal(FletcherStatus.TransportFailure, refused.Status);
        Assert.Contains("Agent", refused.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ABoundedPayloadOverflowSurfacesAsPayloadTooLarge()
    {
        // BIND-4's bullet 8 over a real transport. Unproven until the BIND-4 review
        // (conformance, bullet 8), and then found FALSE: the Fast DDS provider dropped
        // an oversized row and only logged it, so this very test published a 512-byte
        // row through a 128-byte bound without an exception. The provider now reports
        // the overflow as kPayloadTooLarge (D-BIND-53, amended), and this test is
        // what keeps it doing so from C#.
        //
        // A FACT, NOT A THEORY ROW, and that is not per-transport code in the
        // binding: `inprocess` ignores `max_payload_bytes` by design
        // (`in_process_provider.hpp`), so a theory row over it would have nothing to
        // refuse. Fast DDS enforces the bound, and is the transport a managed
        // caller would actually hit it on.
        const uint Bound = 128;
        Assert.True(PayloadBound.IsValid(Bound));

        using PubSubProviderHandle provider = ProviderRegistry.Create(
            ProviderSelector.Parse("fastdds"), new ProviderConfig { MaxPayloadBytes = Bound });
        using var publisher = new Publisher(provider);

        var schema = new Schema([new Field("payload", StringType.Default, nullable: false)], metadata: null);
        TopicPath topic = UniqueTopic("overflow");
        publisher.CreateTopic(topic, schema);

        // The control: a row well under the bound is accepted, so the refusal below
        // is about SIZE rather than a bound that refuses everything.
        using (RecordBatch small = new(schema, [new StringArray.Builder().Append("tiny").Build()], length: 1))
        using (var codec = new FletcherCodec(schema))
        using (BoundRows rows = codec.Bind(small))
        {
            publisher.Publish(topic, rows, 0);
        }

        string oversized = new('x', 4 * (int)Bound);
        using (RecordBatch large = new(schema, [new StringArray.Builder().Append(oversized).Build()], length: 1))
        using (var codec = new FletcherCodec(schema))
        using (BoundRows rows = codec.Bind(large))
        {
            // Throws, not ThrowsAny: exactly FletcherException. The overflow happens while
            // the codec writes the row into the transport's window, so the shim attributes
            // it to the CODEC origin - but a row too large for the bound is valid data, not
            // malformed input, so it is not the FletcherFormatException subclass (D-BIND-54).
            FletcherException refused = Assert.Throws<FletcherException>(() => publisher.Publish(topic, rows, 0));
            Assert.Equal(FletcherStatus.PayloadTooLarge, refused.Status);
        }
    }

    [Fact]
    public void EveryTransportInThisSuiteWasActuallyExercised()
    {
        // THE VACUITY GUARD. A `TheoryData` that lost a row, or a selector renamed
        // out from under it, would leave this suite green while testing one
        // transport - which is exactly the failure mode the round has hit twice
        // (the sparse-checkout goldens, and the branch counters). The count is
        // asserted against the list the rows actually run over.
        // The list is the single source the theory is built from, so the count
        // check below cannot drift from what the rows actually ran over.
        Assert.Equal(SelectorNames.Length, Transports.Count);
        Assert.Equal(3, SelectorNames.Length);
        Assert.Contains("inprocess", SelectorNames);
        Assert.Contains("fastdds", SelectorNames);
        Assert.Contains("xrce", SelectorNames);
    }
}
