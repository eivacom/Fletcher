// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-3a's forcing property: the shim loads from a managed process, and the
// binding refuses a shim it was not built against.
//
// ── Why these rows exist in this order ──────────────────────────────────────
// Everything else in the round's managed half sits on top of one assumption —
// that a .NET process can find and load the native shim at all. Until that is
// asserted, every later test would be testing the codec and the loader at once,
// and a load failure would present as whatever the first codec call happened to
// be. So it is asserted here, first, on its own.
//
// ── These rows FAIL rather than SKIP when the shim is absent ────────────────
// Deliberately. A skipped test reads green, and "the native asset was never
// built" is exactly the condition that must not read green — it is the same
// mistake the shim's own single-copy scan had to avoid, where finding nothing
// looks identical to a healthy process. The failure carries the resolver's
// diagnostic, which names the RID and says to run `conan create c-abi`.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

using Eiva.Fletcher.Interop;
using Eiva.Fletcher.Testing;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class NativeLoaderTests
{
    /// <summary>
    /// The shim loads, and its ABI version is the one this binding was compiled
    /// against.
    /// </summary>
    /// <remarks>
    /// Touching any member of <c>NativeMethods</c> runs its type initializer,
    /// which performs the handshake — so a mismatch fails this row before the
    /// assertion below is even reached, which is the behaviour the header asks
    /// for ("compare at load time").
    /// </remarks>
    [Fact]
    public void TheShimLoadsAndReportsTheAbiVersionThisBindingWasBuiltAgainst()
    {
        uint version = NativeMethods.fl_binding_abi_version();

        Assert.Equal(NativeLoader.HeaderVersion, version);
        Assert.Equal(NativeLoader.HeaderVersionMajor, version >> 16);
        Assert.Equal(NativeLoader.HeaderVersionMinor, version & 0xFFFF);
    }

    /// <summary>
    /// The single-copy marker is reachable through the ABI.
    /// </summary>
    /// <remarks>
    /// Two things at once, and the second is the reason it is here. It is a
    /// second entry point, so the surface is not a single lucky symbol; and
    /// calling it is what the shim itself relies on to keep its load-time
    /// single-copy scan from being dead-stripped by the linker. A managed caller
    /// reaching this function is the closest thing this suite has to evidence
    /// that the scan is present in the shipped artifact.
    /// </remarks>
    [Fact]
    public void TheSingleCopyMarkerIsReachableAndNotEmpty()
    {
        nint marker = NativeMethods.fl_single_copy_marker();

        Assert.NotEqual(0, marker);
        string? text = Marshal.PtrToStringUTF8(marker);
        Assert.False(string.IsNullOrEmpty(text));
        Assert.Contains("fletcher-c-abi", text, StringComparison.Ordinal);
    }

    /// <summary>
    /// The handshake accepts the version it was built against.
    /// </summary>
    [Fact]
    public void TheHandshakeAcceptsItsOwnVersion() =>
        NativeLoader.VerifyAbiVersion(NativeLoader.HeaderVersion);

    /// <summary>
    /// The handshake REFUSES anything else, and this is the row that makes the
    /// one above mean something.
    /// </summary>
    /// <remarks>
    /// A version check nobody has watched reject a version is not a check. The
    /// cases below are the three shapes that matter, and the third is the one a
    /// semantic-versioning habit would wave through: while MAJOR is 0, `binding.h`
    /// gives NO compatibility guarantee at all, so a NEWER minor is refused too.
    /// If this binding ever reaches 1.0, that case becomes legal and this row has
    /// to be revisited on purpose rather than discovered.
    /// </remarks>
    /// <remarks>
    /// DERIVED from the header's own constants rather than written out, and that
    /// is not tidiness: the cases were literals until D-BIND-42 bumped the minor
    /// from 1 to 2, at which point `(0, 2)` — listed here as "a newer minor,
    /// refused" — became the version the shim actually reports, and this row
    /// failed. It failing was the right outcome; it needing a hand-edit on every
    /// ABI bump was not. Each case is now an offset from the real version, so the
    /// next bump moves them with it.
    /// </remarks>
    [Theory]
    [InlineData(NativeLoader.HeaderVersionMajor + 1, NativeLoader.HeaderVersionMinor)]
    [InlineData(NativeLoader.HeaderVersionMajor, NativeLoader.HeaderVersionMinor - 1)]
    [InlineData(NativeLoader.HeaderVersionMajor, NativeLoader.HeaderVersionMinor + 1)]
    public void TheHandshakeRefusesAnyOtherVersion(uint major, uint minor)
    {
        // None of the three is the matching version, by construction: each is an
        // offset from it, and the matching one is asserted by the row above.
        uint packed = (major << 16) | minor;

        InvalidOperationException error =
            Assert.Throws<InvalidOperationException>(() => NativeLoader.VerifyAbiVersion(packed));

        // The message has to name both versions, or its reader cannot tell which
        // half of the pair is the odd one.
        Assert.Contains($"{major}.{minor}", error.Message, StringComparison.Ordinal);
        Assert.Contains(
            $"{NativeLoader.HeaderVersionMajor}.{NativeLoader.HeaderVersionMinor}",
            error.Message,
            StringComparison.Ordinal);
    }

    /// <summary>
    /// The resolver answers for Fletcher's shim and for nothing else.
    /// </summary>
    /// <remarks>
    /// It is installed per-assembly, but it is still a hook in a process that
    /// loads other native libraries. Returning anything but zero for a name that
    /// is not ours would put this package in the way of somebody else's
    /// dependency.
    /// </remarks>
    [Fact]
    public void TheResolverDoesNotAnswerForOtherLibraries()
    {
        nint handle = NativeLoader.Resolve(
            "some-other-native-library",
            typeof(NativeLoader).Assembly,
            searchPath: null);

        Assert.Equal(0, handle);
    }

    /// <summary>
    /// Review G-D1, re-anchored by D10C-4 (audit 0a T42; d10 L5): the shim is looked for beside
    /// this ASSEMBLY first, then beside the application, each both flat and under
    /// <c>runtimes/&lt;rid&gt;/native/</c>; and EVERY server candidate comes before EVERY edge
    /// candidate, so a server file anywhere beside the app wins.
    /// </summary>
    /// <remarks>
    /// A plugin-style host (a PowerShell module, an MSBuild task, a custom
    /// AssemblyLoadContext) loads this assembly from its own folder, where the package's
    /// <c>runtimes</c> tree sits, while the application directory is the host's. A NativeAOT
    /// or single-file app has no assembly directory, so only the application's is probed.
    /// One directory reached both ways (an ordinary app; a trailing separator) is probed once.
    /// Asserted on the pure candidate list: a second copy of the shim cannot be loaded into
    /// this process to prove it end to end, because the single-copy marker refuses it. The
    /// end-to-end proof that the server file wins is LoaderLayoutTests'; this is the order.
    /// Before D10C-4 the list held the edge name alone (4 candidates), which pinned "the
    /// server file is never looked for", against L5.
    /// </remarks>
    [Fact]
    public void TheShimIsLookedForBesideTheAssemblyThenBesideTheApplication()
    {
        string plugin = Path.Combine(Path.GetTempPath(), "host", "plugins", "fletcher");
        string app = Path.Combine(Path.GetTempPath(), "host");
        const string Rid = "test-x64";
        string server = LoadedShim.ServerFileName;
        string edge = LoadedShim.EdgeFileName;
        string[] folders =
        [
            plugin,
            Path.Combine(plugin, "runtimes", Rid, "native"),
            app,
            Path.Combine(app, "runtimes", Rid, "native"),
        ];

        string[] both = NativeLoader.CandidatePaths(plugin, app, Rid).ToArray();
        Assert.Equal(8, both.Length);
        for (int i = 0; i < folders.Length; i++)
        {
            Assert.Equal(Path.Combine(folders[i], server), both[i]);
            Assert.Equal(Path.Combine(folders[i], edge), both[folders.Length + i]);
        }

        // An ordinary app: one directory, probed once, however it is spelled; server first.
        string[] ordinary = NativeLoader.CandidatePaths(app, app + Path.DirectorySeparatorChar, Rid).ToArray();
        Assert.Equal(
            [Path.Combine(folders[2], server), Path.Combine(folders[3], server), Path.Combine(folders[2], edge), Path.Combine(folders[3], edge)],
            ordinary);
        // NativeAOT or single-file: no assembly directory, so the application's alone; server first.
        string[] aot = NativeLoader.CandidatePaths(null, app, Rid).ToArray();
        Assert.Equal(ordinary, aot);
    }

    /// <summary>
    /// D10C-4 (d10 L5, D28; ruling 2026-11-15 "Presence; fail loudly"): only the FIRST present
    /// file is tried. Two present files, both refused: the refusal names the first and never
    /// the second, because the second is never tried.
    /// </summary>
    /// <remarks>
    /// A leg of the no-fallback rule; LoaderLayoutTests.AnUnloadableServerShimStopsTheAppAndLoadsNoShim
    /// is its outside proof. Two refused files rather than one refused and one valid, because a
    /// valid second copy cannot be loaded into this process (the single-copy marker), and the
    /// message is what shows whether the loop went on to it.
    /// </remarks>
    [Fact]
    public void OnlyTheFirstPresentFileIsTried()
    {
        string folder = Path.Combine(Path.GetTempPath(), "fletcher-first-" + Guid.NewGuid().ToString("N"));
        string first = Path.Combine(folder, "a", "libfletcher-c-abi.so");
        string second = Path.Combine(folder, "b", "libfletcher-c-abi.so");
        Directory.CreateDirectory(Path.GetDirectoryName(first)!);
        Directory.CreateDirectory(Path.GetDirectoryName(second)!);
        try
        {
            File.WriteAllText(first, "not a shared library");
            File.WriteAllText(second, "not a shared library");

            DllNotFoundException e = Assert.Throws<DllNotFoundException>(
                () => NativeLoader.LoadFromCandidates([first, second], "linux-x64"));

            Assert.Contains(first + ":", e.Message, StringComparison.Ordinal);
            Assert.DoesNotContain(second, e.Message, StringComparison.Ordinal);
        }
        finally
        {
            Directory.Delete(folder, recursive: true);
        }
    }

    /// <summary>
    /// D10C-4 (design review B1, P6): the package folder is keyed on the OS and the process
    /// architecture, so a distro-built .NET, whose own RID is e.g. <c>ubuntu.24.04-x64</c>,
    /// still looks in <c>runtimes/linux-x64/native/</c>, and is told the linux-x64 floor.
    /// </summary>
    /// <remarks>
    /// A LEG, not the proof (law b): no runtime in CI reports a distro RID. The outside proof is
    /// two-part: LoaderLayoutTests' runtimes arms stage at the literal package folder, and
    /// NoNativeLoadOutsideTheResolver fails on any read of the runtime's RID in
    /// Fletcher.Interop (DEBT-5).
    /// </remarks>
    [Fact]
    public void ADistroBuiltRuntimeStillFindsTheLinuxX64Folder()
    {
        string linux = NativeLoader.PackageRid(OSPlatform.Linux, Architecture.X64);
        Assert.Equal("linux-x64", linux);
        Assert.Equal("win-x64", NativeLoader.PackageRid(OSPlatform.Windows, Architecture.X64));

        string app = Path.Combine(Path.GetTempPath(), "distro-app");
        string package = Path.Combine(app, "runtimes", "linux-x64", "native");
        Assert.Contains(NativeLoader.CandidatePaths(app, app, linux), p => Path.GetDirectoryName(p) == package);
        Assert.Contains("Linux-gcc13", NativeLoader.PlatformFloor(linux), StringComparison.Ordinal);
    }

    /// <summary>
    /// BIND-9b-7 (review T14, D-BIND-172): a shim that is THERE but the operating system will not
    /// load is reported as such, with the operating system's reason and the platform floor, not
    /// as an unsupported RID.
    /// </summary>
    /// <remarks>
    /// Found running the packed consumer on Debian 12, below the floor: the package's shim was in
    /// place and refused for want of GLIBCXX_3.4.32, and the message said the RID 'linux-x64' was
    /// perhaps not shipped. A file of the shim's name that is not a library stands in for a shim
    /// the loader refuses; a missing candidate before it shows that only the present one is
    /// reported as refused.
    /// </remarks>
    [Fact]
    public void AShimThatIsThereButWillNotLoadIsReportedWithTheReasonAndTheFloor()
    {
        string folder = Path.Combine(Path.GetTempPath(), "fletcher-refused-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            string missing = Path.Combine(folder, "absent", "libfletcher-c-abi.so");
            string present = Path.Combine(folder, "libfletcher-c-abi.so");
            File.WriteAllText(present, "not a shared library");

            DllNotFoundException e = Assert.Throws<DllNotFoundException>(
                () => NativeLoader.LoadFromCandidates([missing, present], "linux-x64"));

            Assert.Contains("found its native shim", e.Message, StringComparison.Ordinal);
            Assert.Contains("the operating system could not load it: " + present + ":", e.Message, StringComparison.Ordinal);
            Assert.DoesNotContain(missing + ":", e.Message, StringComparison.Ordinal);
            Assert.Contains("glibc 2.34", e.Message, StringComparison.Ordinal);
            Assert.Contains("GLIBCXX_3.4.32", e.Message, StringComparison.Ordinal);
            Assert.DoesNotContain("if yours is not among them", e.Message, StringComparison.Ordinal);
        }
        finally
        {
            Directory.Delete(folder, recursive: true);
        }
    }

    /// <summary>
    /// When no candidate exists at all, the message is still the one about the RID: that is the
    /// cause when nothing was found (D-BIND-172 keeps it).
    /// </summary>
    [Fact]
    public void NoShimAtAllIsStillReportedAgainstTheRid()
    {
        string missing = Path.Combine(Path.GetTempPath(), "fletcher-absent-" + Guid.NewGuid().ToString("N"), "libfletcher-c-abi.so");

        DllNotFoundException e = Assert.Throws<DllNotFoundException>(
            () => NativeLoader.LoadFromCandidates([missing], "linux-musl-x64"));

        Assert.Contains("for RID 'linux-musl-x64'", e.Message, StringComparison.Ordinal);
        Assert.Contains("if yours is not among them", e.Message, StringComparison.Ordinal);
        Assert.Contains("Searched: " + missing, e.Message, StringComparison.Ordinal);
        Assert.DoesNotContain("found its native shim", e.Message, StringComparison.Ordinal);
    }

    /// <summary>Each shipped RID's floor is the one its build profile sets (D-BIND-172).</summary>
    [Fact]
    public void EachShippedRidStatesTheFloorOfItsBuildProfile()
    {
        Assert.Contains("Linux-gcc13", NativeLoader.PlatformFloor("linux-x64"), StringComparison.Ordinal);
        Assert.Contains("14.40", NativeLoader.PlatformFloor("win-x64"), StringComparison.Ordinal);
        Assert.Contains("Windows-msvc194", NativeLoader.PlatformFloor("win-x64"), StringComparison.Ordinal);
        Assert.Contains("ships no shim", NativeLoader.PlatformFloor("osx-arm64"), StringComparison.Ordinal);
    }

    /// <summary>
    /// D10C-4 (d10 L5; audit 0a watch (a)): this process has exactly one shim loaded, and it is
    /// the server file exactly when a server file is beside the tests, else the edge file; the
    /// shim's own variant query agrees.
    /// </summary>
    /// <remarks>
    /// A leg, run on both of ci.dotnet's legs: on the edge leg (edge file only) it asserts the
    /// edge; on the server leg (both files staged) it asserts the server, so that leg cannot go
    /// green on a silent fallback to the edge, which runs per-row equally well. Which file
    /// LOADED is read from the module list, not from which files are present.
    /// </remarks>
    [Fact]
    public void TheLoadedShimIsTheServerExactlyWhenItIsBesideTheTests()
    {
        FlVariant variant = NativeMethods.fl_binding_variant();
        bool serverBeside = File.Exists(Path.Combine(AppContext.BaseDirectory, LoadedShim.ServerFileName));
        string expected = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            serverBeside ? LoadedShim.ServerFileName : LoadedShim.EdgeFileName));

        IReadOnlyList<string> modules = LoadedShim.Modules();

        Assert.True(
            modules.Count == 1 && string.Equals(Path.GetFullPath(modules[0]), expected, LoadedShim.Comparison),
            $"expected exactly {expected} loaded (a server file is {(serverBeside ? string.Empty : "not ")}beside the " +
            $"tests); loaded: [{string.Join(", ", modules)}]");
        Assert.Equal(serverBeside ? FlVariant.Server : FlVariant.Edge, variant);
    }

    /// <summary>
    /// D10C-4, the BC-10 source scan (d10 L5: one resolver site, no fallback branch): in
    /// <c>dotnet/src</c>, no <c>NativeLibrary.</c> outside <c>NativeLoader.cs</c>, and no
    /// <c>LibraryImport(</c> or <c>DllImport(</c> outside <c>Fletcher.Interop</c>; and (design
    /// review DEBT-5) no read of the runtime's RID anywhere in <c>Fletcher.Interop</c>, since a
    /// distro-built .NET reports a RID no package folder carries.
    /// </summary>
    /// <remarks>
    /// A guard, with vacuity controls: a scan that reads nothing finds nothing, so it fails unless
    /// it finds <c>NativeLoader.cs</c>'s own <c>NativeLibrary.Load(</c>, an import inside
    /// Fletcher.Interop, and the folder key <c>PackageRid(</c>. Code lines only: a line that is
    /// a comment (<c>//</c>, <c>///</c>, or inside a block comment's <c>*</c> lines) is skipped,
    /// so prose may name what the code must not do. The RID check is the identifier
    /// <c>RuntimeIdentifier</c> as a whole word, which also catches a <c>using static</c> read, and the
    /// host property <c>RUNTIME_IDENTIFIER</c>, the <c>AppContext.GetData</c> route to the same value (audit 0b G3).
    /// </remarks>
    [Fact]
    public void NoNativeLoadOutsideTheResolver()
    {
        string src = typeof(NativeLoaderTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
            .FirstOrDefault(a => a.Key == "FletcherDotnetSrc")?.Value ?? string.Empty;
        Assert.True(Directory.Exists(src), $"the baked dotnet/src folder '{src}' does not exist.");
        string interop = Path.GetFullPath(Path.Combine(src, "Fletcher.Interop"));
        string loader = Path.Combine(interop, "NativeLoader.cs");

        Dictionary<string, (int Line, string Text)[]> code = Directory
            .EnumerateFiles(src, "*.cs", SearchOption.AllDirectories)
            .Select(Path.GetFullPath)
            .Where(f => !Path.GetRelativePath(src, f).Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                .Any(part => part is "bin" or "obj"))
            .ToDictionary(f => f, CodeLines, StringComparer.Ordinal);
        bool InInterop(string file) => file.StartsWith(interop + Path.DirectorySeparatorChar, LoadedShim.Comparison);
        bool IsLoader(string file) => string.Equals(file, loader, LoadedShim.Comparison);
        string[] Hits(Func<string, bool> fileFilter, Func<string, bool> lineFilter) => code
            .Where(kv => fileFilter(kv.Key))
            .SelectMany(kv => kv.Value.Where(l => lineFilter(l.Text)).Select(l => $"{kv.Key}:{l.Line}: {l.Text}"))
            .ToArray();

        // Vacuity controls for the BC-10 half.
        Assert.True(
            Hits(IsLoader, l => l.Contains("NativeLibrary.Load(", StringComparison.Ordinal)).Length > 0,
            $"vacuous: the scan of {src} did not find NativeLoader.cs's own NativeLibrary.Load(.");
        Assert.True(
            Hits(InInterop, l => l.Contains("LibraryImport(", StringComparison.Ordinal)).Length > 0,
            $"vacuous: the scan of {src} found no LibraryImport( in Fletcher.Interop.");

        // BC-10: one resolver site.
        string[] strayLoads = Hits(f => !IsLoader(f), l => l.Contains("NativeLibrary.", StringComparison.Ordinal));
        Assert.True(strayLoads.Length == 0, "NativeLibrary. outside NativeLoader.cs:\n" + string.Join("\n", strayLoads));
        string[] strayImports = Hits(
            f => !InInterop(f),
            l => l.Contains("LibraryImport(", StringComparison.Ordinal) || l.Contains("DllImport(", StringComparison.Ordinal));
        Assert.True(strayImports.Length == 0, "a native import outside Fletcher.Interop:\n" + string.Join("\n", strayImports));

        // DEBT-5: the folder key is never the runtime's own RID.
        string[] runtimeRid = Hits(InInterop, l => Regex.IsMatch(l, @"\bRuntimeIdentifier\b|RUNTIME_IDENTIFIER"));
        Assert.True(
            runtimeRid.Length == 0,
            "Fletcher.Interop reads the runtime's RID, which on a distro-built .NET names no package folder:\n" +
            string.Join("\n", runtimeRid));

        // Vacuity control for the DEBT-5 half: the key it must use instead is there.
        Assert.True(
            Hits(InInterop, l => l.Contains("PackageRid(", StringComparison.Ordinal)).Length > 0,
            $"vacuous: the scan of {src} found no PackageRid( in Fletcher.Interop.");
    }

    /// <summary>Each line of <paramref name="file"/> that is code, numbered from 1, trimmed.</summary>
    private static (int Line, string Text)[] CodeLines(string file) => File.ReadAllLines(file)
        .Select((text, index) => (Line: index + 1, Text: text.Trim()))
        .Where(l => l.Text.Length > 0
            && !l.Text.StartsWith("//", StringComparison.Ordinal)
            && !l.Text.StartsWith("/*", StringComparison.Ordinal)
            && !l.Text.StartsWith('*'))
        .ToArray();
}
