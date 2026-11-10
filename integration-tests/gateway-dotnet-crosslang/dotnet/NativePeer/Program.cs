// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The C# native peer of the cross-language lane (BIND-8d, D-BIND-116; BIND-9b-4, D-BIND-161):
// the generated C# native pair (Eiva.Fletcher, the shim's codec and providers) over Fast DDS, on
// the domain of the gateway the C# gateway client talks to. The same protocol as the C++ peer:
//
//   * subscribes at start to every topic it receives; prints `<topic> RECV <seq> OK` or
//     `<topic> RECV <seq> MISMATCH <diffs>` for every row, compared field by field with the
//     fixture row its `seq` names.
//   * declares every topic it publishes at start; on the stdin line `publish <topic>` publishes
//     the four fixture rows on it, then prints `PUBLISHED 4`; on `probe <topic>` it publishes one
//     row no receiver compares (seq -1) and prints `PROBED` (D-BIND-129). Any other topic prints
//     `ERROR`.
//
// Two of them run, so that C# native reaches C# native across processes:
//
//   * `--role primary` (the default) receives CsToNative, CppToNative and TsToNative, and
//     publishes NativeToCs, NativeToCpp, NativeToTs and NativeToNative.
//   * `--role secondary` only receives NativeToNative. The primary does not, so its own rows
//     never reach it in-process, which is what `inprocess` already covers.
//
// Prints READY once every endpoint exists, exits on the stdin line `stop`.
//
// CLI: --domain-id N --document <path of the Fast DDS profile document the gateway loads too>
//      [--role primary|secondary]
using System;
using System.Collections.Generic;
using System.IO;

using Eiva.Fletcher;
using Eiva.Fletcher.CrossLang;
using Eiva.Fletcher.TestFixtures;

using Fletcher.Gen.Gateway.Crosslang;

uint domainId = 0;
byte[] document = [];
string role = "primary";
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
        case "--role":
            role = args[++i];
            break;
    }
}

if (role is not ("primary" or "secondary"))
{
    Console.Error.WriteLine($"unknown --role '{role}'");
    return 2;
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

// Disposed in reverse at exit, subscriptions before their subscribers.
var owned = new Stack<IDisposable>();
var publishers = new Dictionary<string, Action<CrossRow>>();

// Not generic: `dotnet format`'s design-time load runs no protoc, so it cannot infer a type
// argument from a generated class and would report these as never used.
void Receive(string topic, IDisposable subscriber, Func<Action<CrossRow>, Subscription> subscribe)
{
    owned.Push(subscriber);
    owned.Push(subscribe(row => PrintLine($"{topic} {RowCompare.ReportRow(row.Seq, row.Row)}")));
}

void Send(string topic, IDisposable publisher, Action<CrossRow> publish)
{
    owned.Push(publisher);
    publishers[topic] = publish;
}

if (role == "primary")
{
    var csToNative = new CrossLang_CsToNativeSubscriber(provider);
    Receive("CsToNative", csToNative, report => csToNative.Subscribe((row, _) => report(row)));
    var cppToNative = new CrossLang_CppToNativeSubscriber(provider);
    Receive("CppToNative", cppToNative, report => cppToNative.Subscribe((row, _) => report(row)));
    var tsToNative = new CrossLang_TsToNativeSubscriber(provider);
    Receive("TsToNative", tsToNative, report => tsToNative.Subscribe((row, _) => report(row)));

    var toCs = new CrossLang_NativeToCsPublisher(provider);
    Send("NativeToCs", toCs, toCs.Publish);
    var toCpp = new CrossLang_NativeToCppPublisher(provider);
    Send("NativeToCpp", toCpp, toCpp.Publish);
    var toTs = new CrossLang_NativeToTsPublisher(provider);
    Send("NativeToTs", toTs, toTs.Publish);
    var toNative = new CrossLang_NativeToNativePublisher(provider);
    Send("NativeToNative", toNative, toNative.Publish);
}
else
{
    var nativeToNative = new CrossLang_NativeToNativeSubscriber(provider);
    Receive("NativeToNative", nativeToNative, report => nativeToNative.Subscribe((row, _) => report(row)));
}

PrintLine("READY");

while (Console.In.ReadLine() is { } line)
{
    if (line == "stop")
    {
        break;
    }

    string[] words = line.Split(' ', 2);
    if (words[0] is not ("probe" or "publish"))
    {
        continue;
    }

    string topic = words.Length > 1 ? words[1] : "";
    if (!publishers.TryGetValue(topic, out Action<CrossRow>? publish))
    {
        PrintLine($"ERROR no publisher for topic '{topic}'");
        continue;
    }

    if (words[0] == "probe")
    {
        // D-BIND-129: a row no receiver compares (seq -1), sent until one arrives.
        publish(new CrossRow { Seq = -1 });
        PrintLine("PROBED");
    }
    else
    {
        for (int seq = 0; seq < CoverageFixture.CompositeRowCount; seq++)
        {
            publish(new CrossRow { Seq = seq, Row = CoverageFixture.CompositeRow(seq) });
        }

        PrintLine($"PUBLISHED {CoverageFixture.CompositeRowCount}");
    }
}

while (owned.Count > 0)
{
    owned.Pop().Dispose();
}

return 0;
