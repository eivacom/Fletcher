// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Which of Fletcher's native shims this process has LOADED (D10C-4, rung 4).
//
// ── Why the module list, and not the files beside the app ───────────────────
// A file that is present proves nothing about which build answered: the loader
// could have skipped it and loaded another (the forbidden fallback, ruling
// 2026-11-15 "Presence; fail loudly"). The operating system's own list of the
// modules mapped into this process is the outside observation: it names the
// file that was actually loaded, whatever the binding believes. One helper, shared
// by Fletcher.Tests, the test host and the conformance suite, so the three ask the
// question the same way.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;

namespace Eiva.Fletcher.Testing;

internal static class LoadedShim
{
    /// <summary>The edge build's file name on this platform.</summary>
    internal static string EdgeFileName => FileName("fletcher-c-abi");

    /// <summary>The server build's file name on this platform (ruling 2026-11-13, "its own name").</summary>
    internal static string ServerFileName => FileName("fletcher-c-abi-server");

    /// <summary>
    /// The package's native folder, relative to the app, spelled LITERALLY as the NuGet package
    /// lays it out: <c>runtimes/win-x64/native</c> or <c>runtimes/linux-x64/native</c> (D-BIND-27).
    /// </summary>
    /// <remarks>
    /// Never computed from the runtime's RID (design review DEBT-5): a distro-built .NET reports
    /// a distro RID such as <c>ubuntu.24.04-x64</c>, while the package's shim still sits under
    /// <c>linux-x64</c>. Staging at the literal folder is what makes a host run an outside proof
    /// that the package's folder is found.
    /// </remarks>
    internal static string PackageNativeFolder =>
        Path.Combine("runtimes", OperatingSystem.IsWindows() ? "win-x64" : "linux-x64", "native");

    /// <summary>A shim base name decorated as this platform's loader expects a file to be named.</summary>
    internal static string FileName(string baseName) =>
        OperatingSystem.IsWindows() ? baseName + ".dll"
        : OperatingSystem.IsMacOS() ? "lib" + baseName + ".dylib"
        : "lib" + baseName + ".so";

    /// <summary>Every loaded module whose file is one of Fletcher's shims, by full path.</summary>
    internal static IReadOnlyList<string> Modules()
    {
        using Process self = Process.GetCurrentProcess();
        return self.Modules.Cast<ProcessModule>()
            .Select(module => module.FileName)
            .Where(IsShim)
            .ToArray();
    }

    /// <summary>
    /// Whether <paramref name="path"/> names a shipped Fletcher shim, of either build: exactly the
    /// edge or the server file name, so the test-only probe shim is not counted (review 4b N2).
    /// </summary>
    internal static bool IsShim(string path)
    {
        string name = Path.GetFileName(path);
        return string.Equals(name, EdgeFileName, Comparison) || string.Equals(name, ServerFileName, Comparison);
    }

    /// <summary>File names compare as the platform's file system does.</summary>
    internal static StringComparison Comparison =>
        OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
}
