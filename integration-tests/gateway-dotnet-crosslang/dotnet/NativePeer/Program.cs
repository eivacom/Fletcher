// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The C# native peer of the cross-language lane (BIND-8d, D-BIND-116): the generated C# native
// pair (Eiva.Fletcher, the shim's codec and providers) over Fast DDS, on the domain of the
// gateway the C# gateway client talks to. The same protocol as the C++ peer:
//
//   * CsToNative: subscribes at start; prints `RECV <seq> OK` or `RECV <seq> MISMATCH <diffs>`
//     for every row, compared field by field with the fixture row its `seq` names.
//   * NativeToCs: declares the topic at start; publishes the four fixture rows on the stdin
//     line `publish`, then prints `PUBLISHED 4`; on `probe` it publishes one row no receiver
//     compares (seq -1) and prints `PROBED` (D-BIND-129).
//
// Prints READY once both endpoints exist, exits on the stdin line `stop`.
//
// CLI: --domain-id N --document <path of the Fast DDS profile document the gateway loads too>
using System;
using System.IO;

using Eiva.Fletcher;
using Eiva.Fletcher.CrossLang;
using Eiva.Fletcher.TestFixtures;

using Fletcher.Gen.Gateway.Crosslang;

uint domainId = 0;
byte[] document = [];
for (int i = 0; i + 1 < args.Length; i++)
{
    switch (args[i])
    {
        case "--domain-id":
            domainId = uint.Parse(args[++i], System.Globalization.CultureInfo.InvariantCulture);
            break;
        case "--document":
            document = File.ReadAllBytes(args[++i]);
            break;
    }
}

object output = new();
void PrintLine(string line)
{
    lock (output)
    {
        Console.Out.WriteLine(line);
        Console.Out.Flush();
    }
}

using PubSubProviderHandle provider = ProviderRegistry.Create(
    ProviderSelector.Parse("fastdds"),
    new ProviderConfig { DomainId = domainId, Document = document });

using var fromCs = new CrossLang_CsToNativeSubscriber(provider);
Subscription subscription = fromCs.Subscribe((row, _) => PrintLine(RowCompare.ReportRow(row.Seq, row.Row)));

using var toCs = new CrossLang_NativeToCsPublisher(provider);

PrintLine("READY");

while (Console.In.ReadLine() is { } line)
{
    if (line == "stop")
    {
        break;
    }

    if (line == "probe")
    {
        // D-BIND-129: a row no receiver compares (seq -1), sent until one arrives.
        toCs.Publish(new CrossRow { Seq = -1 });
        PrintLine("PROBED");
    }

    if (line == "publish")
    {
        for (int seq = 0; seq < CoverageFixture.CompositeRowCount; seq++)
        {
            toCs.Publish(new CrossRow { Seq = seq, Row = CoverageFixture.CompositeRow(seq) });
        }

        PrintLine($"PUBLISHED {CoverageFixture.CompositeRowCount}");
    }
}

fromCs.Unsubscribe(subscription);
