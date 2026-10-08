// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6a/6b: what the generated model is, as a consumer meets it.
//
// Much of this is proven by compiling at all: the properties below are named by
// D-BIND-69/70/73's rules, and a wrong name would not build. The assertions add what a
// compile cannot see: the namespace ignores csharp_namespace, enum members carry their
// proto numbers, and the model needs nothing of Fletcher's at run time (D-BIND-72).
using System;
using System.Collections.Generic;
using System.Linq;

using Apache.Arrow.Types;

using Eiva.Fletcher.Model;

using Fletcher.Gen.Integration.ProtocDotnet;
using Fletcher.Gen.Integration.SharedTypes;

using Xunit;

namespace Eiva.Fletcher.ProtocDotnet;

public sealed class GeneratedModelTests
{
    [Fact]
    public void TheNamespaceIsFletcherGenPlusThePackageWhateverCsharpNamespaceSays()
    {
        Assert.Equal("Fletcher.Gen.Integration.ProtocDotnet", typeof(Reading).Namespace);
        Assert.Equal("Fletcher.Gen.Integration.ProtocDotnet", typeof(Player_Stats).Namespace);
    }

    [Fact]
    public void PropertiesAreNamedByProtocsRules()
    {
        var player = new Player
        {
            Player_ = "named after its class",
            Class = 7,                   // a C# keyword, legal once PascalCased
            Clone_ = true,               // one of protoc's reserved member names
            Schema_ = "one of ours",     // one of the generated class's own members
            Mode = Player_Mode._2D,      // a nested enum, flat; a member starting with a digit
        };
        Assert.Equal(7, player.Class);
        Assert.True(player.Clone_);
    }

    [Fact]
    public void EnumMembersCarryTheirProtoNumbersAliasesIncluded()
    {
        Assert.Equal(0, (int)Color.Unspecified);
        Assert.Equal(2, (int)Color.DarkBlue);
        Assert.Equal(1, (int)Player_Mode._2D);
        Assert.Equal(1, (int)Player_Mode.Flat);
    }

    [Fact]
    public void TypesFromAnotherFileAreThatFilesClassesInAnotherAssembly()
    {
        // BIND-6e: Route's property types are the classes generated from shared.proto and
        // timestamp.proto, named in their own package's namespace, and they live in
        // ProtocDotnetShared, not here. That is the topology D-BIND-80 is for: were Route
        // to call their internal ToArrowColumns, this project would not compile (CS0117,
        // measured by mutant: from another assembly an internal member is simply absent).
        Type[] other = { typeof(Pos), typeof(Fix), typeof(Quality), typeof(global::Fletcher.Gen.Google.Protobuf.Timestamp) };
        foreach (Type t in other)
        {
            Assert.NotEqual(typeof(Route).Assembly, t.Assembly);
            Assert.Equal("ProtocDotnetShared", t.Assembly.GetName().Name);
        }

        Assert.Equal("Fletcher.Gen.Integration.SharedTypes", typeof(Pos).Namespace);
        Assert.Equal(typeof(Pos), typeof(Route).GetProperty("Start")!.PropertyType);
        Assert.Equal(typeof(List<global::Fletcher.Gen.Google.Protobuf.Timestamp>), typeof(Route).GetProperty("Marks")!.PropertyType);
        Assert.Equal(typeof(List<KeyValuePair<string, Pos>>), typeof(Route).GetProperty("Stops")!.PropertyType);
        Assert.Equal(typeof(List<Quality>), typeof(Route).GetProperty("Checks")!.PropertyType);

        // ToArrowColumns stays internal: the cross-file path did not widen the surface.
        Assert.Null(typeof(Pos).GetMethod("ToArrowColumns", System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static));
        Assert.NotNull(typeof(Pos).GetMethod("ToArrowColumns", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static));

        // The other assembly is a model too: Apache.Arrow, and nothing of Fletcher's at all.
        var references = typeof(Pos).Assembly.GetReferencedAssemblies().Select(a => a.Name ?? "").ToList();
        Assert.Contains("Apache.Arrow", references);
        Assert.DoesNotContain(references, name => name == "Fletcher" || name.StartsWith("Fletcher.", StringComparison.Ordinal));
    }

    [Fact]
    public void TheModelReferencesApacheArrowAndFletcherModelAndNothingElseOfFletchers()
    {
        // D-BIND-72: the model layer compiles against Apache.Arrow alone, so a gateway or
        // WASM application never pulls in native assets. D-BIND-74 adds exactly one
        // Fletcher assembly to that, Fletcher.Model (the package Eiva.Fletcher.Model), which
        // is itself Apache.Arrow only. The compiled result must have acquired no other: not
        // the codec (assembly "Fletcher"), not the interop package ("Fletcher.Interop"), not
        // the gateway client ("Fletcher.GatewayClient").
        //
        // The assemblies are named after their projects, NOT their package ids, so the
        // prefix to look for is "Fletcher". Until BIND-6c this test looked for "Eiva.Fletcher"
        // and could not have failed on any of them.
        var references = typeof(Reading).Assembly.GetReferencedAssemblies().Select(a => a.Name ?? "").ToList();
        Assert.Contains("Apache.Arrow", references);
        Assert.Contains("Fletcher.Model", references);
        Assert.Equal(
            new[] { "Fletcher.Model" },
            references
                .Where(name => name == "Fletcher" || name.StartsWith("Fletcher.", StringComparison.Ordinal))
                .ToArray());
    }

    [Fact]
    public void EveryMappedFieldHasItsPropertyWithTheShapeD_BIND_75Rules()
    {
        // The properties exist before ToArrow / FromArrow convert them (6c), so a consumer
        // can already build and read the model. Each type is checked, not just compiled.
        Assert.Equal(typeof(List<int>), typeof(Player).GetProperty("Scores")!.PropertyType);
        Assert.Equal(typeof(Player_Stats), typeof(Player).GetProperty("Stats")!.PropertyType);
        Assert.Equal(typeof(List<Player_Stats>), typeof(Player).GetProperty("History")!.PropertyType);
        Assert.Equal(
            typeof(List<KeyValuePair<string, int>>), typeof(Player).GetProperty("Tags")!.PropertyType);
        Assert.Equal(typeof(Timestamp), typeof(Timed).GetProperty("At")!.PropertyType);
        Assert.Equal(typeof(Duration), typeof(Timed).GetProperty("Took")!.PropertyType);
        Assert.Equal(typeof(Timestamp?), typeof(Timed).GetProperty("MaybeAt")!.PropertyType);
        Assert.Equal(typeof(int?), typeof(Timed).GetProperty("Boxed")!.PropertyType);
    }

    [Fact]
    public void CollectionsStartEmptyAndAMessageFieldStartsAbsent()
    {
        var player = new Player();

        Assert.Empty(player.Scores);
        Assert.Empty(player.Tags);
        Assert.Empty(player.History);
        Assert.Null(player.Stats);
        // A Timestamp field is always present, as its non-nullable Arrow column says: the
        // default is the epoch in seconds, not null.
        Assert.Equal(default(Timestamp), new Timed().At);
    }

    [Fact]
    public void AMapKeepsItsEntryOrderAndItsDuplicates()
    {
        // D-BIND-75: a Dictionary would lose both, and the Arrow map keeps both on the
        // wire, as C++'s vector of pairs does.
        var player = new Player();
        player.Tags.Add(new KeyValuePair<string, int>("b", 2));
        player.Tags.Add(new KeyValuePair<string, int>("a", 1));
        player.Tags.Add(new KeyValuePair<string, int>("b", 3));

        Assert.Equal(new[] { "b", "a", "b" }, player.Tags.Select(t => t.Key).ToArray());
        Assert.Equal(new[] { 2, 1, 3 }, player.Tags.Select(t => t.Value).ToArray());
    }

    [Fact]
    public void ATimestampKeepsItsNanosecondDigitsInTheModel()
    {
        // D-BIND-26: the generated property is the lossless type, not a 100 ns DateTime.
        var timed = new Timed
        {
            At = new Timestamp(1_700_000_000_123_456_789L, TimeUnit.Nanosecond),
            Took = new Duration(1_999L, TimeUnit.Nanosecond),
        };

        Assert.Equal(1_700_000_000_123_456_789L, timed.At.Value);
        Assert.Equal(1_999L, timed.Took.Value);
    }
}
