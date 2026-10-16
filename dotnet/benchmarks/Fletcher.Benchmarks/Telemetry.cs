// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The ONE row every arm publishes, C++ and C#, and the bytes it must become.
//
// The schema is written by hand because no generated C# exists before BIND-6,
// and the generator's own IPC goldens carry no `Telemetry`. Hand-written is safe
// here for one reason only: `CanonicalRowHex` is the SAME constant
// `c-abi/benchmarks/bench_publish.cpp` pins from the generated C++ publisher, and
// `Validation` refuses to time anything unless this schema and these values put
// exactly those bytes on the wire. A drift in either would be a stopped run, not
// a quietly incomparable column.
using System;

using Apache.Arrow;
using Apache.Arrow.Types;

namespace Eiva.Fletcher.Benchmarks;

internal static class Telemetry
{
    internal const int DeviceId = 42;
    internal const double Value = 3.14;
    internal const long Timestamp = 1700000000000;
    internal const string MetricName = "cpu.temperature";

    /// <summary>The generated C++ publisher's topic, segment for segment.</summary>
    internal static readonly TopicPath Topic = TopicPath.Of("integration.pubsub", "TelemetryFeed", "TelemetryStream");

    /// <summary>Pinned in bench_publish.cpp's <c>kCanonicalRowHex</c>. Change both or neither.</summary>
    internal const string CanonicalRowHex =
        "002a0000001f85eb51b81e09400068e5cf8b0100000f0000006370752e74656d7065726174757265";

    internal static readonly byte[] CanonicalRow = Convert.FromHexString(CanonicalRowHex);

    /// <summary><c>integration/protoc-arrow-bridge/proto/pubsub.proto</c>'s <c>Telemetry</c>.</summary>
    internal static readonly Schema Schema = new Schema.Builder()
        .Field(f => f.Name("device_id").DataType(Int32Type.Default).Nullable(false))
        .Field(f => f.Name("value").DataType(DoubleType.Default).Nullable(false))
        .Field(f => f.Name("timestamp").DataType(Int64Type.Default).Nullable(false))
        .Field(f => f.Name("metric_name").DataType(StringType.Default).Nullable(false))
        .Build();

    /// <summary><paramref name="rows"/> copies of the canonical row.</summary>
    /// <remarks>
    /// This is ALSO the body of M2's per-publish cost: an application holding one
    /// row's values and wanting it on the wire has to build exactly this.
    /// </remarks>
    internal static RecordBatch Batch(int rows)
    {
        var deviceId = new Int32Array.Builder().Reserve(rows);
        var value = new DoubleArray.Builder().Reserve(rows);
        var timestamp = new Int64Array.Builder().Reserve(rows);
        var metricName = new StringArray.Builder().Reserve(rows);

        for (int i = 0; i < rows; i++)
        {
            deviceId.Append(DeviceId);
            value.Append(Value);
            timestamp.Append(Timestamp);
            metricName.Append(MetricName);
        }

        return new RecordBatch(
            Schema,
            [deviceId.Build(), value.Build(), timestamp.Build(), metricName.Build()],
            rows);
    }
}
