// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Finding the shim, and refusing to talk to the wrong one.
//
// ── Why a resolver at all ───────────────────────────────────────────────────
// The default probing rules find the native asset once the package is installed:
// NuGet lays `runtimes/<rid>/native/` out beside the app and the host adds it to
// the search path. What they do NOT give is a good failure. A missing RID comes
// out as `DllNotFoundException: Unable to load shared library
// 'fletcher-c-abi'`, followed by a list of the dozen names the loader tried -
// none of which tells the reader the one thing that is wrong, which is that
// their RID is not one this package ships. So the resolver exists mostly for the
// message it produces when it fails.
using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;

namespace Eiva.Fletcher.Interop;

/// <summary>Locates the native shim and verifies its ABI version.</summary>
internal static class NativeLoader
{
    /// <summary>
    /// The ABI version this assembly was built against, mirrored from
    /// <c>binding.h</c>'s <c>FL_BINDING_ABI_VERSION_MAJOR</c>.
    /// </summary>
    internal const uint HeaderVersionMajor = 0;

    /// <summary>Mirrored from <c>FL_BINDING_ABI_VERSION_MINOR</c>.</summary>
    internal const uint HeaderVersionMinor = 7;

    /// <summary>The header's version packed the way the shim reports its own.</summary>
    internal const uint HeaderVersion = (HeaderVersionMajor << 16) | HeaderVersionMinor;

    /// <summary>
    /// Install the resolver. Called once, from <see cref="NativeMethods"/>'s type
    /// initializer, before the first P/Invoke can resolve through it.
    /// </summary>
    /// <remarks>
    /// NOT a module initializer, though that was the first shape tried. CA2255
    /// refuses one in a library and is right to: a module initializer runs when
    /// the assembly is first touched, which for a library is a moment the
    /// CONSUMER does not control and cannot order. A type initializer on the
    /// class holding the imports runs at the only moment that matters — just
    /// before the first call through them — which is also what `binding.h` says
    /// this assembly does.
    ///
    /// Installing a resolver twice for one assembly throws, so this must have
    /// exactly one caller. A type initializer gives that for free.
    /// </remarks>
    internal static void Install() =>
        NativeLibrary.SetDllImportResolver(typeof(NativeLoader).Assembly, Resolve);

    /// <summary>The resolver itself. Internal so its refusal to answer for other
    /// libraries can be asserted without loading anything.</summary>
    internal static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        // Ours only. Returning zero for anything else leaves every other library
        // in the process on the default rules, which is what a resolver installed
        // per-assembly must do.
        if (!string.Equals(libraryName, NativeMethods.LibraryName, StringComparison.Ordinal))
        {
            return IntPtr.Zero;
        }

        // The ordinary route first, because in a published application it is the
        // correct one and it is what the package's `runtimes/<rid>/native` layout
        // is designed to satisfy.
        if (NativeLibrary.TryLoad(libraryName, assembly, searchPath, out IntPtr handle))
        {
            return handle;
        }

        // The development route. There is no NuGet layout in a source build, so
        // the shim is staged beside the test binary or under a `runtimes` folder
        // by the build; probing both keeps `dotnet test` working from a plain
        // checkout without a packaging step. It is also where an installed
        // package's shim is found again when the ordinary route failed, so the
        // failure can say why.
        return LoadFromCandidates(
            CandidatePaths(AssemblyDirectory(assembly), AppContext.BaseDirectory, RuntimeInformation.RuntimeIdentifier),
            RuntimeInformation.RuntimeIdentifier);
    }

    /// <summary>Load the first candidate that exists and loads; otherwise throw, saying why.</summary>
    /// <remarks>
    /// Thrown, not returned as zero. Returning zero hands the caller the runtime's own
    /// message, which lists what it tried and never names the cause. Two causes are told
    /// apart (BIND-9b-7, review T14, D-BIND-172):
    /// <list type="bullet">
    ///   <item>No candidate exists: the RID is not one the package ships, or a source build
    ///   did not stage the shim.</item>
    ///   <item>A candidate exists and the operating system refused it: almost always a machine
    ///   below the platform floor the shim is built for (an older glibc or C++ runtime, a
    ///   missing Visual C++ redistributable). The operating system's own reason is quoted,
    ///   because it names the missing piece (for example "version `GLIBCXX_3.4.32' not
    ///   found"), and the floor is stated beside it. Blaming the RID here, as this did before,
    ///   sends the reader looking for a platform that is in fact supported.</item>
    /// </list>
    /// Internal, with the candidates passed in, so a test can hand it a file that exists and
    /// cannot load.
    /// </remarks>
    internal static IntPtr LoadFromCandidates(IEnumerable<string> candidates, string rid)
    {
        List<string> probed = [];
        List<string> refused = [];
        foreach (string candidate in candidates)
        {
            probed.Add(candidate);
            if (!File.Exists(candidate))
            {
                continue;
            }

            try
            {
                return NativeLibrary.Load(candidate);
            }
            catch (Exception e) when (e is DllNotFoundException or BadImageFormatException)
            {
                refused.Add($"{candidate}: {e.Message}");
            }
        }

        string alsoProbed = probed.Count == 0 ? string.Empty : " Also probed: " + string.Join("; ", probed) + ".";
        if (refused.Count > 0)
        {
            throw new DllNotFoundException(
                $"Fletcher found its native shim '{NativeMethods.LibraryName}' for RID '{rid}', but the " +
                "operating system could not load it: " + string.Join("; ", refused) + ". This almost " +
                "always means the machine is below the platform floor the shim is built for: " +
                PlatformFloor(rid) + "." + alsoProbed);
        }

        throw new DllNotFoundException(
            $"Fletcher could not load its native shim '{NativeMethods.LibraryName}' for RID " +
            $"'{rid}'. Eiva.Fletcher.Interop ships one native " +
            "asset per supported RID; if yours is not among them the package cannot run on this " +
            "platform, and no managed fallback exists because the wire codec is native by design " +
            "(D-BIND-1). In a source build, the shim is produced by `conan create c-abi` and must " +
            "be staged beside the test binary." + alsoProbed);
    }

    /// <summary>The platform floor of the shim for <paramref name="rid"/>, as the build profile that
    /// produces it sets it (D-BIND-172).</summary>
    /// <remarks>
    /// The numbers are <c>scripts/shim_floors.env</c>'s and the package README's, and
    /// <c>scripts/check_dotnet_packages.sh</c> requires this file to state the same ones, so a
    /// floor that moves fails the pack until it moves here too.
    /// </remarks>
    internal static string PlatformFloor(string rid) => rid switch
    {
        "linux-x64" => "on linux-x64, glibc 2.34 and the GCC 13 C++ runtime (libstdc++ with GLIBCXX_3.4.32) " +
            "or newer, as the Linux-gcc13 build profile produces it, for example Ubuntu 24.04, Debian 13 or RHEL 10",
        "win-x64" => "on win-x64, the Visual C++ 2015-2022 redistributable (x64), 14.40 or newer, as the " +
            "Windows-msvc194 build profile requires",
        _ => $"Eiva.Fletcher.Interop ships no shim for '{rid}', so this file is not one of the package's",
    };

    /// <summary>
    /// The directory of <paramref name="assembly"/>'s file, or <see langword="null"/> when it
    /// has none.
    /// </summary>
    /// <remarks>
    /// In a NativeAOT or single-file app an assembly has no file and <c>Assembly.Location</c>
    /// is empty, which is what trim warning IL3000 warns of (found by BIND-6e's NativeAOT
    /// publish). That case is handled here, by answering null, and the application's directory
    /// is probed instead, so the warning is suppressed for this one read.
    /// </remarks>
    [UnconditionalSuppressMessage("SingleFile", "IL3000:Avoid accessing Assembly file path when publishing as a single file",
        Justification = "An empty Location (NativeAOT, single-file) is answered with null; AppContext.BaseDirectory is probed as well.")]
    private static string? AssemblyDirectory(Assembly assembly)
    {
        string location = assembly.Location;
        return string.IsNullOrEmpty(location) ? null : Path.GetDirectoryName(location);
    }

    /// <summary>Where a source build, or a host that loads this assembly as a plugin, might
    /// have put the shim, in the order probed.</summary>
    /// <remarks>
    /// Beside this ASSEMBLY first, then beside the APPLICATION (review G-D1). In an ordinary
    /// app the two are one directory, probed once. In a plugin-style host (a PowerShell module,
    /// an MSBuild task, a custom <c>AssemblyLoadContext</c>) the package's
    /// <c>runtimes/&lt;rid&gt;/native/</c> sits beside the assembly, not the host, so only the
    /// first finds it; in a NativeAOT or single-file app the assembly has no directory and only
    /// the second applies. Internal and pure so the order can be asserted without loading.
    /// </remarks>
    internal static IEnumerable<string> CandidatePaths(string? assemblyDirectory, string? appDirectory, string rid)
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (string? directory in new[] { assemblyDirectory, appDirectory })
        {
            if (string.IsNullOrEmpty(directory)
                || !seen.Add(Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory))))
            {
                continue;
            }

            foreach (string name in NativeFileNames())
            {
                yield return Path.Combine(directory, name);
                yield return Path.Combine(directory, "runtimes", rid, "native", name);
            }
        }
    }

    /// <summary>
    /// The shim's file name under each platform's own convention.
    /// </summary>
    /// <remarks>
    /// Spelled out because these probes load by PATH, and a path has to carry the
    /// decoration that <see cref="NativeMethods.LibraryName"/> deliberately omits.
    /// </remarks>
    private static IEnumerable<string> NativeFileNames()
    {
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            yield return NativeMethods.LibraryName + ".dll";
        }
        else if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
        {
            yield return "lib" + NativeMethods.LibraryName + ".dylib";
        }
        else
        {
            yield return "lib" + NativeMethods.LibraryName + ".so";
        }
    }

    /// <summary>
    /// Compare the loaded shim's ABI version against the one this assembly was
    /// built against, and throw if they cannot work together.
    /// </summary>
    /// <remarks>
    /// The rule is `binding.h`'s own, and it is stricter than semantic versioning
    /// looks:
    /// <list type="bullet">
    ///   <item>The MAJOR must match. Across a major, declarations may be removed.</item>
    ///   <item><b>While MAJOR is 0, the MINOR must match EXACTLY.</b> The header
    ///   states plainly that before 1.0 there is no compatibility guarantee at all
    ///   and a binding is expected to be rebuilt against the shim it ships with,
    ///   so accepting a different minor here would be accepting a surface that may
    ///   have changed shape.</item>
    ///   <item>From 1.0, append-only within a major makes a NEWER minor safe: the
    ///   shim may have grown declarations this binding does not call. An OLDER
    ///   minor is still refused — this binding may call something absent.</item>
    /// </list>
    /// </remarks>
    internal static void VerifyAbiVersion(uint shimVersion)
    {
        uint shimMajor = shimVersion >> 16;
        uint shimMinor = shimVersion & 0xFFFF;

        bool compatible = shimMajor == HeaderVersionMajor
            && (HeaderVersionMajor == 0 ? shimMinor == HeaderVersionMinor : shimMinor >= HeaderVersionMinor);

        if (!compatible)
        {
            throw new InvalidOperationException(
                $"Fletcher's native shim reports binding ABI {shimMajor}.{shimMinor}, and this build of " +
                $"Eiva.Fletcher.Interop was compiled against {HeaderVersionMajor}.{HeaderVersionMinor}. " +
                (HeaderVersionMajor == 0
                    ? "Before 1.0 the ABI carries no compatibility guarantee, so the two must match exactly: "
                      + "the managed packages and the native shim are versioned and shipped together, and a "
                      + "mismatch means one of them came from somewhere else."
                    : "Within a major version the shim may be newer than the binding but never older."));
        }
    }
}
