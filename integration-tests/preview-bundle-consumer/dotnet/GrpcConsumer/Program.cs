// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-9b-5 (D-BIND-158): Grpc.Tools compiled reading.proto, which imports fletcher/options.proto,
// and Eiva.Fletcher.Protoc compiled options.proto beside it. The exit code is the verdict: 0 only
// when both options read back as set and a message round-trips.
using System;

using Eiva.Fletcher.Protobuf;

using Google.Protobuf;

using GrpcConsumer.Messages;

bool flatten = Celsius.Descriptor.GetOptions().GetExtension(OptionsExtensions.Flatten);
bool flattenField = Reading.Descriptor.FindFieldByName("temperature").GetOptions().GetExtension(OptionsExtensions.FlattenField);
var sent = new Reading { Sensor = "a", Temperature = new Celsius { Value = 21.5 } };
Reading back = Reading.Parser.ParseFrom(sent.ToByteArray());

Console.WriteLine($"(fletcher.flatten) on Celsius: {flatten}");
Console.WriteLine($"(fletcher.flatten_field) on Reading.temperature: {flattenField}");
Console.WriteLine($"round trip: {back}");
if (!flatten || !flattenField || !back.Equals(sent))
{
    Console.Error.WriteLine("GrpcConsumer: the options did not read back as set, or the message did not round-trip");
    return 1;
}

Console.WriteLine("GrpcConsumer: OK");
return 0;
