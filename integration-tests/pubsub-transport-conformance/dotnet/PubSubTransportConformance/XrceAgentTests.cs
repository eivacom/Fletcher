// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D-BIND-56: the XRCE half of bucket 4 needs an Agent, and a harness that starts
// one must be able to REFUSE one. These are that ability's two forcing tests -
// twins, in the same words, of `AForeignAgentDoesNotSatisfyTheHarness` and
// `AFailedOwnershipQueryDoesNotSatisfyTheHarness` in the C++ interop and
// conformance harnesses - and the one case that crosses the Agent's bridge in
// the direction a C# workstation meets an XRCE device.
using System;
using System.Buffers;
using System.Threading;

using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.TransportConformance;

[Collection(XrceAgentCollection.Name)]
public sealed class XrceAgentTests
{
    // Taken so xUnit starts the suite's Agent before any case here runs.
    public XrceAgentTests(XrceAgentFixture agent) => ArgumentNullException.ThrowIfNull(agent);

    /// <summary>An Agent that answers, but that another process holds the port for, is refused.</summary>
    /// <remarks>
    /// Two Agents on the contested port: the first is ours and proven; the second
    /// dies of `bind error` within milliseconds while the FIRST answers its probe.
    /// A harness that asked only "does an Agent answer?" certifies the second.
    /// Fails against a fixture that skips <c>ProveOwnership</c>.
    /// </remarks>
    [Fact]
    public void AForeignAgentDoesNotSatisfyTheHarness()
    {
        using XrceAgent incumbent = XrceAgent.Start(XrceAgent.ContestedPort);

        InvalidOperationException refused =
            Assert.Throws<InvalidOperationException>(() => XrceAgent.Start(XrceAgent.ContestedPort).Dispose());

        Assert.Contains($"UDP {XrceAgent.ContestedPort} is held by another process", refused.Message, StringComparison.Ordinal);
    }

    /// <summary>An ownership question the OS could not answer is a refusal, not a pass.</summary>
    /// <remarks>
    /// The Agent is genuinely alive and genuinely ours; only the query fails. An
    /// earlier C++ revision fell back to liveness here and PASSED. Fails against a
    /// fixture that treats <see cref="PortOwnership.QueryFailed"/> as good enough.
    /// </remarks>
    [Fact]
    public void AFailedOwnershipQueryDoesNotSatisfyTheHarness()
    {
        var real = UdpPortOwnership.Query;
        UdpPortOwnership.Query = (_, _) => (PortOwnership.QueryFailed, "a stub that fails, on purpose");
        try
        {
            InvalidOperationException refused =
                Assert.Throws<InvalidOperationException>(() => XrceAgent.Start(XrceAgent.ContestedPort).Dispose());

            Assert.Contains("could not ask the OS who holds UDP", refused.Message, StringComparison.Ordinal);
            Assert.Contains("a stub that fails, on purpose", refused.Message, StringComparison.Ordinal);
        }
        finally
        {
            UdpPortOwnership.Query = real;
        }
    }

    /// <summary>A row a C# client publishes over XRCE reaches a C# Fast DDS subscriber through the Agent.</summary>
    /// <remarks>
    /// The deployment path the round's requirements name (Feature 16353): a device
    /// speaks XRCE-DDS to an Agent, which bridges into the DDS network a
    /// workstation reads with Fast DDS. Both ends are C# over the one shim, on a
    /// domain of their own. The BYTES are compared against the codec's own
    /// encoding - an Agent that mangled the envelope would still deliver a row.
    /// </remarks>
    [Fact]
    public void ARowPublishedOverXrceReachesAFastDdsSubscriberThroughTheAgent()
    {
        const uint Domain = 157;
        using PubSubProviderHandle device = ProviderRegistry.Create(
            ProviderSelector.Parse("xrce"), XrceAgentFixture.ClientConfig(Domain));
        using PubSubProviderHandle workstation = ProviderRegistry.Create(
            ProviderSelector.Parse("fastdds"), new ProviderConfig { DomainId = Domain });
        using var publisher = new Publisher(device);
        using var subscriber = new Subscriber(workstation);

        var schema = new Schema([new Field("id", Int32Type.Default, nullable: true)], metadata: null);
        using RecordBatch batch = new(schema, [new Int32Array.Builder().Append(42).Build()], length: 1);
        using var codec = new FletcherCodec(schema);
        using BoundRows rows = codec.Bind(batch);

        var expected = new ArrayBufferWriter<byte>();
        codec.Encode(rows, 0, expected);

        TopicPath topic = TopicPath.Of("bind4", "bridge", Guid.NewGuid().ToString("N"));
        publisher.CreateTopic(topic, schema);

        using var seen = new ManualResetEventSlim(false);
        byte[]? received = null;
        SubscribeResult result = subscriber.Subscribe(topic, (row, _, _) =>
        {
            received ??= row.ToArray();
            seen.Set();
        });

        using (result.Schema)
        {
            bool arrived = CrossTransportTests.PublishUntilSeen(() => publisher.Publish(topic, rows, 0), seen);
            Assert.True(arrived, "no row crossed XRCE -> Agent -> Fast DDS within the deadline");
        }

        Assert.Equal(expected.WrittenSpan.ToArray(), received);
        result.Subscription.Dispose();
    }
}
