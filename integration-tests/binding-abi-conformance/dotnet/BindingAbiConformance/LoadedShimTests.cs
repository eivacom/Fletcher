// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D10C-4 (re-anchors T47; d9 L13, d10 L5): which build this suite actually ran against.
//
// The lane runs the corpus twice, once with the edge file beside the tests and once with the
// server file beside them UNDER ITS OWN NAME (ruling 2026-11-15 "Distinct names, loader prefers";
// the renamed copy of 2026-11-14 retires). A file beside the tests proves nothing about which
// build answered, so this row reads the operating system's module list: exactly one Fletcher
// shim is loaded, it is the one beside the tests, and it is the server file exactly when a
// server file is there. The same rule on both passes; on the edge pass it asserts the edge.
using System;
using System.Collections.Generic;
using System.IO;

using Eiva.Fletcher.Testing;

using Xunit;

namespace Eiva.Fletcher.Conformance;

public sealed class LoadedShimTests
{
    [Fact]
    public void TheShimBesideTheTestsIsTheOneLoaded()
    {
        bool serverBeside = File.Exists(Path.Combine(AppContext.BaseDirectory, LoadedShim.ServerFileName));
        string expected = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            serverBeside ? LoadedShim.ServerFileName : LoadedShim.EdgeFileName));

        // Any public call through the shim loads it; this one needs nothing staged but the shim.
        using (ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig()))
        {
        }

        IReadOnlyList<string> modules = LoadedShim.Modules();
        Assert.True(
            modules.Count == 1 && string.Equals(Path.GetFullPath(modules[0]), expected, LoadedShim.Comparison),
            $"expected exactly {expected} loaded (a server file is {(serverBeside ? string.Empty : "not ")}beside the " +
            $"tests); loaded: [{string.Join(", ", modules)}]");
    }
}
