// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6a/6b: what the generated model is, as a consumer meets it.
//
// Much of this is proven by compiling at all: the properties below are named by
// D-BIND-69/70/73's rules, and a wrong name would not build. The assertions add what a
// compile cannot see: the namespace ignores csharp_namespace, enum members carry their
// proto numbers, and the model needs nothing of Fletcher's at run time (D-BIND-72).
using System.Linq;

using Fletcher.Gen.Integration.ProtocDotnet;

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
    public void TheModelReferencesNothingOfFletchers()
    {
        // D-BIND-72: the model layer compiles against Apache.Arrow alone, so a gateway or
        // WASM application never pulls in native assets. This project references no
        // Fletcher assembly; the compiled result must not have acquired one.
        var references = typeof(Reading).Assembly.GetReferencedAssemblies().Select(a => a.Name ?? "").ToList();
        Assert.Contains("Apache.Arrow", references);
        Assert.DoesNotContain(references, name => name.StartsWith("Eiva.Fletcher", System.StringComparison.Ordinal));
    }
}
