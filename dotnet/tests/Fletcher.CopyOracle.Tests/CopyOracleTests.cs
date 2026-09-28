// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-5b - the copy oracle, run with the C# producer. This is BIND-5's
// acceptance bullet as D-BIND-58 amended it and D-BIND-61 made scorable.
//
// ── What is measured, and through what ──────────────────────────────────────
// This assembly loads the PROBE shim (`fletcher-c-abi-probe`, staged under the
// shipped shim's name) and selects its `probe` provider by name. Everything
// between the test and the probe is the code that ships: `Publisher.Publish`,
// `Eiva.Fletcher.Interop`, the C Data export, `fl_publisher_publish_row`, the
// codec. The probe encodes into a window that records where every byte came
// from, delivers that window itself to the C# `Subscriber`, and the copy
// oracle's own `Judge()` scores it - the same function every C++ leg uses.
//
// ── The two claims ──────────────────────────────────────────────────────────
//   ROWS: `encode_copies == 0` - the payload reached the window appended
//     straight from the RecordBatch's own buffer, with no managed intermediate
//     and no copy in the export - and `row_copies == 0`, the C# handler's span
//     IS that window. Negative controls, each exactly 1 (D-BIND-61): a STAGED
//     publish (`FletcherCodec.Encode` into managed memory, then `PublishRaw`)
//     and a COPIED EXPORT (the same bytes at a second address).
//   ATTACHMENTS: the DELIVERY view (D-BIND-58) - a handler's `AttachmentsView`
//     is the transport's own bytes, by address. Publishing attachments from C#
//     copies by ruling (D-BIND-44), so no managed attachment is published here.
//
// ── The row ─────────────────────────────────────────────────────────────────
// One non-null binary field, so the payload is the one run of bytes with a
// provenance to trace: scalars are read by value and appended from a stack
// temporary, so an address says nothing about them (D-BIND-61's bound).
using System;
using System.Buffers;
using System.Runtime.InteropServices;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher.Interop;

using Xunit;

namespace Eiva.Fletcher.CopyOracle.Tests;

public sealed unsafe class CopyOracleTests : IDisposable
{
    private static readonly int PayloadBytes = 59;

    private static readonly Schema OneBinary =
        new([new Field("payload", BinaryType.Default, nullable: false)], metadata: null);

    private static readonly TopicPath Topic = TopicPath.Of("bind5", "oracle");

    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly Subscriber _subscriber;
    private readonly FletcherCodec _codec = new(OneBinary);

    private int _deliveries;
    private byte[] _rowBytes = [];
    private ProbeMethods.Delivery _delivery;

    public CopyOracleTests()
    {
        _provider = ProviderRegistry.Create(ProviderSelector.Parse("probe"), new ProviderConfig());
        _publisher = new Publisher(_provider);
        _subscriber = new Subscriber(_provider);
        _publisher.CreateTopic(Topic, OneBinary);
        _subscriber.Subscribe(Topic, OnRow).Schema.Dispose();
    }

    public void Dispose()
    {
        _codec.Dispose();
        _subscriber.Dispose();
        _publisher.Dispose();
        _provider.Dispose();
    }

    /// <summary>Sampled INSIDE the delivery, while every pointer is still borrowed.</summary>
    private void OnRow(ReadOnlySpan<byte> row, SchemaHandle schema, AttachmentsView attachments)
    {
        _deliveries++;
        _rowBytes = row.ToArray();
        fixed (byte* at = row)
        {
            _delivery.Row = at;
            _delivery.RowLen = (nuint)row.Length;
        }

        if (attachments.TryFind("loaned"u8, out ReadOnlySpan<byte> loaned))
        {
            fixed (byte* at = loaned)
            {
                _delivery.Loaned = at;
                _delivery.LoanedLen = (nuint)loaned.Length;
            }
        }
    }

    private static RecordBatch OneRow()
    {
        byte[] bytes = new byte[PayloadBytes];
        for (int i = 0; i < bytes.Length; i++)
        {
            bytes[i] = (byte)((i * 31) + 7);
        }

        return new RecordBatch(OneBinary, [new BinaryArray.Builder().Append(bytes.AsSpan()).Build()], length: 1);
    }

    /// <summary>
    /// The payload's address in the batch's OWN buffer. Taken while the batch is
    /// BOUND, so the export holds it where it is for as long as the score needs it.
    /// </summary>
    private static byte* PayloadOf(RecordBatch batch)
    {
        ReadOnlySpan<byte> value = ((BinaryArray)batch.Column(0)).GetBytes(0);
        fixed (byte* at = value)
        {
            return at;
        }
    }

    private ProbeMethods.Verdict Score(byte* payload)
    {
        ProviderHandle handle = _provider.Handle;
        bool added = false;
        handle.DangerousAddRef(ref added);
        try
        {
            FlError err = default;
            int status = ProbeMethods.fl_test_probe_score(
                handle.DangerousGetHandle(), payload, (nuint)PayloadBytes, in _delivery, out ProbeMethods.Verdict verdict, &err);
            Errors.ThrowIfFailed(status, ref err);
            return verdict;
        }
        finally
        {
            if (added)
            {
                handle.DangerousRelease();
            }
        }
    }

    private nuint Loan(ReadOnlySpan<byte> bytes)
    {
        ProviderHandle handle = _provider.Handle;
        bool added = false;
        handle.DangerousAddRef(ref added);
        try
        {
            ReadOnlySpan<byte> key = "loaned"u8;
            fixed (byte* k = key)
            fixed (byte* b = bytes)
            {
                FlError err = default;
                var flKey = new ProbeMethods.Str { Data = k, Len = (nuint)key.Length };
                int status = ProbeMethods.fl_test_probe_loan(
                    handle.DangerousGetHandle(), flKey, b, (nuint)bytes.Length, out nuint baseAddress, &err);
                Errors.ThrowIfFailed(status, ref err);
                return baseAddress;
            }
        }
        finally
        {
            if (added)
            {
                handle.DangerousRelease();
            }
        }
    }

    /// <summary>The acceptance row: C#'s fused publish copies nothing on the way in.</summary>
    [Fact]
    public void TheFusedPublishWritesThePayloadStraightFromTheBatch()
    {
        using RecordBatch batch = OneRow();
        using BoundRows rows = _codec.Bind(batch);
        byte* payload = PayloadOf(batch);

        _publisher.Publish(Topic, rows, 0);

        Assert.Equal(1, _deliveries);
        ProbeMethods.Verdict verdict = Score(payload);
        Assert.Equal(0, verdict.EncodeCopies);
        Assert.Equal((nuint)payload, verdict.ProducedAt);
        Assert.Equal(0UL, verdict.RowCopies);
        Assert.Equal((ulong)(PayloadBytes + 5), verdict.EncodeLen);
    }

    /// <summary>
    /// D-BIND-61's staging control. The row is composed in managed memory and
    /// handed over whole - exactly the failure the acceptance row rules out - and
    /// the provider half cannot see it: only the source can.
    /// </summary>
    [Fact]
    public void AStagedManagedPublishIsCaught()
    {
        using RecordBatch batch = OneRow();
        using BoundRows rows = _codec.Bind(batch);
        byte* payload = PayloadOf(batch);

        var staged = new ArrayBufferWriter<byte>();
        _codec.Encode(rows, 0, staged);
        _publisher.PublishRaw(Topic, staged.WrittenSpan);

        Assert.Equal(1, _deliveries);
        Assert.Equal(staged.WrittenSpan.ToArray(), _rowBytes);
        ProbeMethods.Verdict verdict = Score(payload);
        Assert.Equal(1, verdict.EncodeCopies);
        Assert.Equal(0UL, verdict.RowCopies);
    }

    /// <summary>
    /// D-BIND-61's copied-export control: the same bytes at a second address, as
    /// an export that copied its buffers would leave them. Content cannot tell
    /// the two apart; provenance does, and the verdict names the real source.
    /// </summary>
    [Fact]
    public void ACopiedExportIsCaught()
    {
        using RecordBatch original = OneRow();
        using RecordBatch copy = OneRow();
        using BoundRows originalRows = _codec.Bind(original);
        using BoundRows copyRows = _codec.Bind(copy);
        byte* payload = PayloadOf(original);
        byte* copied = PayloadOf(copy);
        Assert.NotEqual((nuint)payload, (nuint)copied);

        _publisher.Publish(Topic, copyRows, 0);

        ProbeMethods.Verdict verdict = Score(payload);
        Assert.Equal(1, verdict.EncodeCopies);
        Assert.Equal((nuint)copied, verdict.ProducedAt);
    }

    /// <summary>
    /// The attachments claim, as D-BIND-58 set it: a handler's
    /// <see cref="AttachmentsView"/> is the transport's own bytes, by address.
    /// </summary>
    [Fact]
    public void AttachmentsViewHandsTheHandlerTheTransportsOwnBytes()
    {
        byte[] loaned = new byte[1024];
        for (int i = 0; i < loaned.Length; i++)
        {
            loaned[i] = (byte)i;
        }

        nuint parked = Loan(loaned);

        using RecordBatch batch = OneRow();
        using BoundRows rows = _codec.Bind(batch);
        byte* payload = PayloadOf(batch);
        _publisher.Publish(Topic, rows, 0);

        Assert.Equal(parked, (nuint)_delivery.Loaned);
        ProbeMethods.Verdict verdict = Score(payload);
        Assert.Equal(0UL, verdict.AttachmentCopies);
        Assert.Equal(0, verdict.EncodeCopies);
    }

    /// <summary>
    /// The attachment claim's own control: a delivery that did not hand over the
    /// transport's bytes scores as a copy. The same Judge(), fed the address of a
    /// managed copy of what arrived, must say so.
    /// </summary>
    [Fact]
    public void AnAttachmentCopiedOutOfTheDeliveryIsCaught()
    {
        byte[] loaned = new byte[1024];
        Loan(loaned);

        using RecordBatch batch = OneRow();
        using BoundRows rows = _codec.Bind(batch);
        byte* payload = PayloadOf(batch);
        _publisher.Publish(Topic, rows, 0);

        byte[] kept = new ReadOnlySpan<byte>(_delivery.Loaned, (int)_delivery.LoanedLen).ToArray();
        fixed (byte* at = kept)
        {
            _delivery.Loaned = at;
            Assert.Equal(1UL, Score(payload).AttachmentCopies);
        }
    }
}

/// <summary>
/// The probe shim's `fl_test_*` surface (`c-abi/probe/include/fletcher/abi/test/probe.h`),
/// declared HERE and nowhere in <c>Eiva.Fletcher.Interop</c>: the shipped shim
/// does not export it, and the shipped interop assembly must not name it.
/// </summary>
/// <remarks>
/// `fl_error` goes by POINTER and `fl_str` as a local mirror: the source generator
/// will not vouch for the blittability of a struct declared in another assembly,
/// and a pointer needs no vouching. Both are layout-identical to the C.
/// </remarks>
internal static unsafe partial class ProbeMethods
{
    /// <summary>`fl_str`, mirrored.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct Str
    {
        internal byte* Data;
        internal nuint Len;
    }

    /// <summary>`fl_test_delivery`.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct Delivery
    {
        internal byte* Row;
        internal nuint RowLen;
        internal byte* Loaned;
        internal nuint LoanedLen;
    }

    /// <summary>`fl_test_verdict`. `EncodeCopies` is -1 when the producer half was never sampled.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct Verdict
    {
        internal long EncodeCopies;
        internal ulong RowCopies;
        internal ulong AttachmentCopies;
        internal nuint ProducedAt;
        internal nuint EncodeBase;
        internal ulong EncodeLen;
    }

    [LibraryImport(NativeMethods.LibraryName)]
    internal static partial int fl_test_probe_loan(
        nint provider, Str key, byte* bytes, nuint len, out nuint baseAddress, FlError* err);

    [LibraryImport(NativeMethods.LibraryName)]
    internal static partial int fl_test_probe_score(
        nint provider, byte* payload, nuint payloadLen, in Delivery delivery, out Verdict verdict, FlError* err);
}
