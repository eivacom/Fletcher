// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D10C-4: an application on Eiva.Fletcher's public API, run from a staged folder.
//
//   dotnet Fletcher.TestHost.dll
//
// Prints, one per line:
//   delivered                      the row published on `inprocess` came back through
//                                  SubscriberArrow.Subscribe
//   error <type>: <message>        for each exception in the chain, outermost first (exit 3)
//   module <path>                  for each Fletcher shim this process has loaded, always,
//                                  whether the run succeeded or not
// The exit code is 0 on delivery, 1 when the row did not come back, 3 on an exception.
using System;
using System.Threading;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher;
using Eiva.Fletcher.Testing;

int exitCode = 0;
try
{
    var schema = new Schema([new Field("x", Int32Type.Default, nullable: true)], metadata: null);
    TopicPath topic = TopicPath.Of("d10c4", "loader", "host");

    using PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
    using var publisher = new Publisher(provider);
    using var subscriber = new SubscriberArrow(provider);
    publisher.CreateTopic(topic, schema);

    using var arrived = new ManualResetEventSlim();
    int? value = null;
    SubscribeResult result = subscriber.Subscribe(topic, (batch, attachments, status) =>
    {
        if (batch is { Length: 1 })
        {
            value = ((Int32Array)batch.Column(0)).GetValue(0);
            arrived.Set();
        }
    });
    result.Schema.Dispose();

    using (var row = new RecordBatch(schema, [new Int32Array.Builder().Append(42).Build()], length: 1))
    using (var codec = new FletcherCodec(schema))
    using (BoundRows rows = codec.Bind(row))
    {
        publisher.Publish(topic, rows, 0);
    }

    if (arrived.Wait(TimeSpan.FromSeconds(10)) && value == 42)
    {
        Console.WriteLine("delivered");
    }
    else
    {
        Console.WriteLine("not delivered");
        exitCode = 1;
    }
}
catch (Exception e)
{
    for (Exception? link = e; link is not null; link = link.InnerException)
    {
        Console.WriteLine($"error {link.GetType().FullName}: {link.Message.ReplaceLineEndings(" ")}");
    }

    exitCode = 3;
}
finally
{
    foreach (string module in LoadedShim.Modules())
    {
        Console.WriteLine("module " + module);
    }
}

return exitCode;
