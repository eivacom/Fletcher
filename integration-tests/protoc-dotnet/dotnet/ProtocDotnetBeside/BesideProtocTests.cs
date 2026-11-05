// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6e: protoc's C# and Fletcher's C# for the same .proto files, in one assembly.
//
// The main proof is that this project compiles at all: both generators' types for beside.proto
// and shared.proto sit in one assembly, and a collision would be CS0101 (a type defined twice in
// one namespace) at build time. The cases add what a compile cannot see: which namespace each
// generator chose, and that each side still WORKS beside the other, protoc's messages through
// the protobuf wire format and Fletcher's through Arrow.
using System;
using System.IO;
using System.Linq;
using System.Reflection;

using Apache.Arrow;
using Apache.Arrow.Types;

using Google.Protobuf;

using Xunit;

using FletcherPos = Fletcher.Gen.Integration.SharedTypes.Pos;
using FletcherQuality = Fletcher.Gen.Integration.SharedTypes.Quality;
using FletcherReading = Fletcher.Gen.Integration.Beside.Reading;
using FletcherRoute = Fletcher.Gen.Integration.Beside.Route;
using FletcherTimestamp = Fletcher.Gen.Google.Protobuf.Timestamp;
using ProtocPos = Integration.SharedTypes.Pos;
using ProtocQuality = Integration.SharedTypes.Quality;
using ProtocReading = Eiva.Integration.Beside.Reading;
using ProtocRoute = Eiva.Integration.Beside.Route;
using ProtocTimestamp = Google.Protobuf.WellKnownTypes.Timestamp;

namespace Eiva.Fletcher.ProtocDotnetBeside;

public sealed class BesideProtocTests
{
    [Fact]
    public void BothGeneratorsTypesLiveInOneAssemblyEachInItsOwnNamespace()
    {
        // beside.proto sets csharp_namespace: protoc follows it, Fletcher ignores it (D-BIND-69).
        // shared.proto sets none: protoc uses the PascalCased package, Fletcher prefixes
        // Fletcher.Gen. Either way the two never meet.
        Assert.Same(typeof(FletcherReading).Assembly, typeof(ProtocReading).Assembly);
        Assert.Same(typeof(FletcherPos).Assembly, typeof(ProtocPos).Assembly);
        Assert.Equal("Fletcher.Gen.Integration.Beside", typeof(FletcherReading).Namespace);
        Assert.Equal("Eiva.Integration.Beside", typeof(ProtocReading).Namespace);
        Assert.Equal("Fletcher.Gen.Integration.SharedTypes", typeof(FletcherPos).Namespace);
        Assert.Equal("Integration.SharedTypes", typeof(ProtocPos).Namespace);

        // Same short names, different types: the case where a shared namespace would be CS0101.
        Assert.Equal(typeof(FletcherReading).Name, typeof(ProtocReading).Name);
        Assert.NotEqual(typeof(FletcherReading), typeof(ProtocReading));

        // A nested message is flat for Fletcher (Reading_Note, D-BIND-69) and nested for protoc
        // (Reading.Types.Note); an enum keeps its name and member names in both.
        Assert.Equal("Reading_Note", typeof(global::Fletcher.Gen.Integration.Beside.Reading_Note).Name);
        Assert.Equal(typeof(ProtocReading.Types), typeof(ProtocReading.Types.Note).DeclaringType);
        Assert.Equal(
            Enum.GetNames<global::Eiva.Integration.Beside.Mood>(),
            Enum.GetNames<global::Fletcher.Gen.Integration.Beside.Mood>());

        // Every Fletcher type in this assembly is under Fletcher.Gen, so none can land in a
        // namespace protoc writes to.
        string[] fletcherNamespaces = typeof(FletcherReading).Assembly.GetTypes()
            .Where(t => t.GetMethod("ToArrow", BindingFlags.Public | BindingFlags.Static) is not null)
            .Select(t => t.Namespace ?? "")
            .Distinct()
            .ToArray();
        Assert.NotEmpty(fletcherNamespaces);
        Assert.All(fletcherNamespaces, ns => Assert.StartsWith("Fletcher.Gen.", ns, StringComparison.Ordinal));
    }

    [Fact]
    public void TheFileNamesDifferSoOneOutputDirectoryHoldsBoth()
    {
        // One protoc call wrote both generators' files into one directory. On Windows, names
        // compare without case, so Beside.cs and beside.fletcher.cs must differ by more than case.
        string dir = typeof(BesideProtocTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
            .Single(a => a.Key == "FletcherGenDir").Value!;
        string[] names = Directory.GetFiles(dir, "*.cs", SearchOption.AllDirectories)
            .Select(p => Path.GetRelativePath(dir, p).Replace('\\', '/'))
            .Order(StringComparer.Ordinal)
            .ToArray();
        Assert.Equal(
            new[] { "Beside.cs", "Shared.cs", "beside.fletcher.cs", "google/protobuf/timestamp.fletcher.cs", "shared.fletcher.cs" },
            names);
        Assert.Equal(names.Length, names.Distinct(StringComparer.OrdinalIgnoreCase).Count());
    }

    [Fact]
    public void ProtocsRouteStillRoundTripsThroughTheProtobufWireFormat()
    {
        var sent = new ProtocRoute
        {
            Start = new ProtocPos { Lat = 55.67, Lon = 12.57, Label = "start" },
            Quality = ProtocQuality.Poor,
            Marks = { new ProtocTimestamp { Seconds = 1_700_000_000, Nanos = 5 } },
        };
        sent.Legs.Add(new ProtocPos { Lat = -1 });
        sent.Stops.Add("a", new ProtocPos { Lon = 2 });
        sent.Checks.Add(ProtocQuality.Good);

        ProtocRoute back = ProtocRoute.Parser.ParseFrom(sent.ToByteArray());

        Assert.Equal(sent, back);
    }

    [Fact]
    public void FletchersRouteStillRoundTripsThroughArrow()
    {
        var sent = new FletcherRoute
        {
            Start = new FletcherPos { Lat = 55.67, Lon = 12.57, Label = "start" },
            Legs = { new FletcherPos { Lat = -1 } },
            Stops = { new("a", new FletcherPos { Lon = 2 }) },
            Quality = FletcherQuality.Poor,
            Checks = { FletcherQuality.Good },
            Marks = { new FletcherTimestamp { Seconds = 1_700_000_000, Nanos = 5 } },
        };

        using RecordBatch batch = FletcherRoute.ToArrow(new[] { sent });
        FletcherRoute back = FletcherRoute.FromArrow(
            new StructArray(new StructType(batch.Schema.FieldsList), batch.Length, batch.Arrays, ArrowBuffer.Empty, 0), 0);

        Assert.Equal(sent.Start!.Label, back.Start!.Label);
        Assert.Equal(-1, back.Legs.Single().Lat);
        Assert.Equal("a", back.Stops.Single().Key);
        Assert.Equal(FletcherQuality.Poor, back.Quality);
        Assert.Equal(FletcherQuality.Good, back.Checks.Single());
        Assert.Equal((1_700_000_000L, 5), (back.Marks.Single().Seconds, back.Marks.Single().Nanos));
    }
}
