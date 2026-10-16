// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6e, D-BIND-81: what a NativeAOT consumer of the generated pair does, end to end.
//
// Each row crosses everything a trimmer could break: the generated ToArrow and FromArrow
// (lists, a map, a nested message, a lossless Timestamp, and Marks' list of a class from
// another file, built through its public ToArrow, D-BIND-80), the codec behind the C ABI, the
// provider, and the generated subscriber's delegate. The exit code is the verdict: 0 only
// when exactly the rows that were sent come back.
using System;
using System.Collections.Generic;
using System.Linq;

using Apache.Arrow.Types;

using Eiva.Fletcher;
using Eiva.Fletcher.Model;

using Fletcher.Gen.Golden.Pair;

using PbTimestamp = Fletcher.Gen.Google.Protobuf.Timestamp;

using PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());

var seen = new List<string>();
using var readings = new Telemetry_ReportSubscriber(provider);
using Subscription readingSubscription = readings.Subscribe((row, attachments) =>
    seen.Add($"{row.Id} {row.Name} {row.Value} {row.Mood} {string.Join(',', row.Samples)} "
        + $"{row.Tags.Count} {row.Stamp?.By} {row.History.Count} {row.At.WithUnit(TimeUnit.Nanosecond).Value}"));
using var marks = new Telemetry_MarkSubscriber(provider);
using Subscription marksSubscription = marks.Subscribe((row, attachments) =>
    seen.Add("marks " + string.Join(',', row.At.ConvertAll(t => $"{t.Seconds}:{t.Nanos}"))));

using (var publisher = new Telemetry_ReportPublisher(provider))
{
    publisher.Publish(new Reading
    {
        Id = 7,
        Name = "aot",
        Value = 0.5,
        Mood = Mood.Happy,
        Samples = { 1, -2 },
        Tags = { new("a", 1), new("a", 2) },
        Stamp = new Stamp { By = "x" },
        History = { new Stamp { By = "h" } },
        At = new Timestamp(1_700_000_000_123_456_789L, TimeUnit.Nanosecond),
    });
}

using (var publisher = new Telemetry_MarkPublisher(provider))
{
    publisher.Publish(new[] { new Marks { At = { new PbTimestamp { Seconds = 5, Nanos = 6 }, new PbTimestamp { Seconds = -1, Nanos = 0 } } } });
}

string[] expected =
{
    "7 aot 0.5 Happy 1,-2 2 x 1 1700000000123456789",
    "marks 5:6,-1:0",
};

foreach (string line in seen)
{
    Console.WriteLine(line);
}

if (!seen.SequenceEqual(expected))
{
    Console.Error.WriteLine("AotSmoke: expected" + Environment.NewLine + string.Join(Environment.NewLine, expected));
    return 1;
}

Console.WriteLine("AotSmoke: OK");
return 0;
