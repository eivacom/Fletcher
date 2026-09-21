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
    internal const uint HeaderVersionMinor = 3;

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
        // checkout without a packaging step.
        List<string> probed = [];
        foreach (string candidate in CandidatePaths())
        {
            probed.Add(candidate);
            if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out handle))
            {
                return handle;
            }
        }

        // Thrown, not returned as zero. Returning zero hands the caller the
        // runtime's own message, which lists what it tried and never mentions the
        // RID - and the RID is the answer nine times out of ten.
        throw new DllNotFoundException(
            $"Fletcher could not load its native shim '{NativeMethods.LibraryName}' for RID " +
            $"'{RuntimeInformation.RuntimeIdentifier}'. Eiva.Fletcher.Interop ships one native " +
            "asset per supported RID; if yours is not among them the package cannot run on this " +
            "platform, and no managed fallback exists because the wire codec is native by design " +
            "(D-BIND-1). In a source build, the shim is produced by `conan create c-abi` and must " +
            "be staged beside the test binary." +
            (probed.Count == 0 ? string.Empty : " Also probed: " + string.Join("; ", probed) + "."));
    }

    /// <summary>Where a source build might have staged the shim.</summary>
    private static IEnumerable<string> CandidatePaths()
    {
        string? beside = Path.GetDirectoryName(typeof(NativeLoader).Assembly.Location);
        if (string.IsNullOrEmpty(beside))
        {
            // A single-file or in-memory assembly has no location to be beside.
            yield break;
        }

        string rid = RuntimeInformation.RuntimeIdentifier;
        foreach (string name in NativeFileNames())
        {
            yield return Path.Combine(beside, name);
            yield return Path.Combine(beside, "runtimes", rid, "native", name);
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
