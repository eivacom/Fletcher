// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D10C-4, rung 4: which native shim a C# app loads, observed from OUTSIDE the binding.
//
// d10 L5, as ruled 2026-11-15 ("Presence; fail loudly"): a server shim present beside the app
// is loaded; one present but unloadable stops the app loudly; there is no fallback. Each row
// stages a copy of Fletcher.TestHost (an app on the public API only) in a fresh folder with
// the shim files a layout calls for, runs it as its own process (one shim per process, d9
// L17), and reads which file the operating system LOADED from the host's `module` lines. A
// file being present proves nothing; only the module list says which build answered.
//
// The two layouts are the loose one (flat beside the app) and the NuGet one. The NuGet folder
// is spelled LITERALLY, runtimes/win-x64/native or runtimes/linux-x64/native, as the package
// lays it out, never from the runtime's RID (design review DEBT-5): so on Linux the runtimes
// arms prove from outside that the package's folder is found whatever RID the runtime reports.
//
// The edge file comes from beside this test binary (FletcherNativeShim stages it); the server
// file from the baked FletcherServerShim path. With no server path baked, the server rows FAIL
// rather than skip: "the server build was never handed in" must not read green.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;

using Eiva.Fletcher.Testing;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class LoaderLayoutTests
{
    /// <summary>
    /// The forcing row: with both files beside the app, in either layout each, the server file
    /// is the one loaded, and the only one.
    /// </summary>
    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public void TheServerShimLoadsWhenPresentBesideTheApp(bool edgeInRuntimes, bool serverInRuntimes)
    {
        using var app = new StagedApp();
        string edge = app.Stage(EdgeShim, edgeInRuntimes);
        string server = app.Stage(ServerShim, serverInRuntimes);

        HostRun run = app.Run();

        Assert.True(
            run.Modules.Count == 1 && app.SameFile(run.Modules[0], server),
            $"expected exactly the server file loaded, {server}; edge staged at {edge}.\n{run}");
        Assert.True(run.ExitCode == 0 && run.Delivered, $"the app did not run on the server shim.\n{run}");
    }

    /// <summary>
    /// The no-fallback control (audit 0a, watch (b)): a server file that is present but is not a
    /// library, beside a VALID edge file. The app stops, naming the server file, and no shim is
    /// loaded: the edge file, which would load, is never tried.
    /// </summary>
    [Fact]
    public void AnUnloadableServerShimStopsTheAppAndLoadsNoShim()
    {
        using var app = new StagedApp();
        string edge = app.Stage(EdgeShim, inRuntimes: false);
        string server = app.WriteUnloadable(LoadedShim.ServerFileName);

        HostRun run = app.Run();

        Assert.True(run.Modules.Count == 0, $"a shim was loaded although the present server file is unloadable (edge staged at {edge}).\n{run}");
        Assert.True(run.ExitCode == 3, $"the app did not stop on the unloadable server file.\n{run}");
        Assert.False(run.Delivered, $"a row was delivered with no shim loadable.\n{run}");
        Assert.True(
            run.Errors.Any(e => e.StartsWith("System.DllNotFoundException: ", StringComparison.Ordinal)
                && app.Names(e, server)),
            $"no DllNotFoundException naming the server file {server}.\n{run}");
    }

    /// <summary>
    /// Compliance review 4a F1 (ruling 2026-11-15 R-1 (a), "no fallback branch"; d10 L5, D28): a
    /// server file whose presence the app cannot check is CHOSEN, never read as absent. Its folder
    /// comes first in the candidate order and cannot be read, and a valid edge file comes next; the
    /// app stops naming the server file, and the edge file is not loaded.
    /// </summary>
    /// <remarks>
    /// <c>File.Exists</c> answers <c>false</c>, with no error, for a path it cannot examine, so a
    /// loader that asks it walks on to the edge file: a present server file the app cannot load
    /// would end in the edge build. The unreadable folder is made per platform. On Windows, the
    /// folder denies this user the right to list it and to read its files' attributes. Elsewhere,
    /// the folder is a symbolic link to itself, which cannot be resolved (ELOOP); a permission bit
    /// would not do, because the CI container runs as root, which ignores permission bits. The
    /// first assertion checks the premise: from this process, <c>File.Exists</c> on the server path
    /// really answers <c>false</c>.
    /// </remarks>
    [Fact]
    public void AServerFileWhosePresenceCannotBeCheckedIsChosenNotSkipped()
    {
        using var app = new StagedApp();
        string edge = app.Stage(EdgeShim, inRuntimes: false);
        string server = app.StageUnreadableServer(ServerShim);
        try
        {
            Assert.False(File.Exists(server), $"premise: File.Exists must answer false for {server}, which cannot be examined.");

            HostRun run = app.Run();

            Assert.True(run.Modules.Count == 0, $"a shim was loaded although the server file's presence could not be checked (edge staged at {edge}).\n{run}");
            Assert.True(run.ExitCode == 3, $"the app did not stop on the server file it could not check.\n{run}");
            Assert.True(
                run.Errors.Any(e => e.StartsWith("System.DllNotFoundException: ", StringComparison.Ordinal) && app.Names(e, server)),
                $"no DllNotFoundException naming the server file {server}.\n{run}");
        }
        finally
        {
            app.RestoreReadable();
        }
    }

    /// <summary>
    /// The guard (L7): an app with only the edge file runs on it, in either layout. Green before
    /// and after D10C-4.
    /// </summary>
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void AnAppWithOnlyTheEdgeShimRunsOnIt(bool edgeInRuntimes)
    {
        using var app = new StagedApp();
        string edge = app.Stage(EdgeShim, edgeInRuntimes);

        HostRun run = app.Run();

        Assert.True(
            run.Modules.Count == 1 && app.SameFile(run.Modules[0], edge),
            $"expected exactly the edge file loaded, {edge}.\n{run}");
        Assert.True(run.ExitCode == 0 && run.Delivered, $"the app did not run on the edge shim.\n{run}");
    }

    /// <summary>
    /// Audit 0b G1 (d10 L5, D28 "its default probing included"; ruling 2026-11-15 R-1 (a), "one
    /// resolver site, no fallback branch"): a shim reachable only through the operating
    /// system's search path, PATH on Windows or LD_LIBRARY_PATH on Linux, is never loaded. The app
    /// folder holds no shim, so the app stops with the no-shim refusal and nothing is loaded.
    /// </summary>
    /// <remarks>
    /// The outside proof that the runtime's by-name probing is gone: a by-name load kept anywhere
    /// in the resolver, before or after the candidates, finds this file and turns the row red.
    /// Disclosed in the stage brief (an app that relied on PATH now stops at start-up).
    /// </remarks>
    [Fact]
    public void AShimOnlyOnTheOsSearchPathIsNeverLoaded()
    {
        using var app = new StagedApp();
        string onPath = app.StageOutsideTheApp(EdgeShim);

        HostRun run = app.Run(osSearchFolder: Path.GetDirectoryName(onPath));

        Assert.True(run.Modules.Count == 0, $"a shim was loaded from the OS search path ({onPath}), not from the app's folders.\n{run}");
        Assert.True(run.ExitCode == 3, $"the app did not stop with no shim beside it.\n{run}");
        Assert.True(
            run.Errors.Any(e => e.StartsWith("System.DllNotFoundException: ", StringComparison.Ordinal)),
            $"no DllNotFoundException for the missing shim.\n{run}");
    }

    private static string Metadata(string key) =>
        typeof(LoaderLayoutTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
            .FirstOrDefault(a => a.Key == key)?.Value ?? string.Empty;

    /// <summary>The edge file staged beside this test binary.</summary>
    private static string EdgeShim
    {
        get
        {
            string path = Path.Combine(AppContext.BaseDirectory, LoadedShim.EdgeFileName);
            Assert.True(File.Exists(path), $"no edge shim beside the tests at {path}; build with -p:FletcherNativeShim=<path>.");
            return path;
        }
    }

    /// <summary>The server file the build was handed.</summary>
    private static string ServerShim
    {
        get
        {
            string path = Metadata("FletcherServerShim");
            Assert.True(
                path.Length > 0 && File.Exists(path),
                $"no server shim was handed in (got '{path}'); build with -p:FletcherServerShim=<path to {LoadedShim.ServerFileName}>.");
            return path;
        }
    }

    /// <summary>A copy of the test host's output in a fresh folder, and the shims staged into it.</summary>
    private sealed class StagedApp : IDisposable
    {
        private readonly string _root;

        public StagedApp()
        {
            string host = Metadata("FletcherTestHostDir");
            Assert.True(
                Directory.Exists(host) && File.Exists(Path.Combine(host, _hostDll)),
                $"the test host was not built at '{host}'.");

            Leaf = "fletcher-layout-" + Guid.NewGuid().ToString("N");
            _root = Path.Combine(Path.GetTempPath(), Leaf);
            try
            {
                foreach (string file in Directory.EnumerateFiles(host, "*", SearchOption.AllDirectories))
                {
                    Assert.False(LoadedShim.IsShim(file), $"the test host's output holds a shim, {file}; it must hold none.");
                    string target = Path.Combine(_root, Path.GetRelativePath(host, file));
                    Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                    File.Copy(file, target);
                }
            }
            catch
            {
                // No Dispose runs for a constructor that throws, so the partial copy goes here.
                Dispose();
                throw;
            }
        }

        private const string _hostDll = "Fletcher.TestHost.dll";

        /// <summary>The staged folder's own name, unique to this row.</summary>
        private string Leaf { get; }

        /// <summary>Copy <paramref name="shim"/> in under its own name, flat or in the package folder.</summary>
        public string Stage(string shim, bool inRuntimes)
        {
            string folder = inRuntimes ? Path.Combine(_root, LoadedShim.PackageNativeFolder) : _root;
            Directory.CreateDirectory(folder);
            string target = Path.Combine(folder, Path.GetFileName(shim));
            File.Copy(shim, target);
            return target;
        }

        /// <summary>
        /// Copy <paramref name="shim"/> into a folder under this row's root that is NOT one the
        /// loader searches (neither the app folder nor its package folder).
        /// </summary>
        public string StageOutsideTheApp(string shim)
        {
            string folder = Path.Combine(_root, "not-a-candidate");
            Directory.CreateDirectory(folder);
            string target = Path.Combine(folder, Path.GetFileName(shim));
            File.Copy(shim, target);
            return target;
        }

        /// <summary>A file of the shim's name that is not a library, flat beside the app.</summary>
        public string WriteUnloadable(string fileName)
        {
            string target = Path.Combine(_root, fileName);
            File.WriteAllText(target, "not a shared library");
            return target;
        }

        /// <summary>
        /// Whether a loaded module is the staged file: the same path from this row's own folder
        /// down. Compared from the folder's unique name rather than as whole paths, so a temp
        /// folder reached through a link (the OS reports the resolved path) still matches.
        /// </summary>
        public bool SameFile(string module, string staged) =>
            Path.GetFullPath(module).EndsWith(Path.DirectorySeparatorChar + Tail(staged), LoadedShim.Comparison);

        /// <summary>
        /// Whether <paramref name="text"/> names the staged file, matched from this row's own folder
        /// down (review 4b S2): the host may report the temp folder under another spelling, such as
        /// the runner's 8.3-shortened <c>C:\Users\RUNNER~1\...</c>.
        /// </summary>
        public bool Names(string text, string staged) =>
            text.Contains(Path.DirectorySeparatorChar + Tail(staged), LoadedShim.Comparison);

        private string Tail(string staged) => Path.Combine(Leaf, Path.GetRelativePath(_root, staged));

        /// <summary>
        /// Put <paramref name="server"/>'s name in the package folder and make that folder impossible
        /// to examine (see AServerFileWhosePresenceCannotBeCheckedIsChosenNotSkipped). Returns the
        /// server candidate's path. <see cref="RestoreReadable"/> undoes it.
        /// </summary>
        public string StageUnreadableServer(string server)
        {
            string folder = Path.Combine(_root, LoadedShim.PackageNativeFolder);
            string target = Path.Combine(folder, Path.GetFileName(server));
            if (OperatingSystem.IsWindows())
            {
                Directory.CreateDirectory(folder);
                File.Copy(server, target);
                var info = new DirectoryInfo(folder);
                System.Security.AccessControl.DirectorySecurity acl = info.GetAccessControl();
                acl.AddAccessRule(DenyRule());
                info.SetAccessControl(acl);
            }
            else
            {
                Directory.CreateDirectory(Path.GetDirectoryName(folder)!);
                Directory.CreateSymbolicLink(folder, Path.GetFileName(folder));
            }

            _unreadable = folder;
            return target;
        }

        /// <summary>Make the folder <see cref="StageUnreadableServer"/> made unreadable removable again.</summary>
        public void RestoreReadable()
        {
            if (_unreadable is not null && OperatingSystem.IsWindows())
            {
                var info = new DirectoryInfo(_unreadable);
                System.Security.AccessControl.DirectorySecurity acl = info.GetAccessControl();
                acl.RemoveAccessRule(DenyRule());
                info.SetAccessControl(acl);
            }

            _unreadable = null;
        }

        private string? _unreadable;

        [System.Runtime.Versioning.SupportedOSPlatform("windows")]
        private static System.Security.AccessControl.FileSystemAccessRule DenyRule() => new(
            System.Security.Principal.WindowsIdentity.GetCurrent().User!,
            System.Security.AccessControl.FileSystemRights.ListDirectory | System.Security.AccessControl.FileSystemRights.ReadAttributes,
            System.Security.AccessControl.InheritanceFlags.ContainerInherit | System.Security.AccessControl.InheritanceFlags.ObjectInherit,
            System.Security.AccessControl.PropagationFlags.None,
            System.Security.AccessControl.AccessControlType.Deny);

        /// <summary>
        /// Run the host from this folder; <paramref name="osSearchFolder"/>, when given, is put
        /// first on the OS library search path (PATH on Windows, LD_LIBRARY_PATH elsewhere).
        /// </summary>
        public HostRun Run(string? osSearchFolder = null)
        {
            string muxer = Environment.GetEnvironmentVariable("DOTNET_HOST_PATH") is { Length: > 0 } hostPath
                ? hostPath
                : "dotnet";
            var start = new ProcessStartInfo(muxer)
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                WorkingDirectory = _root,
            };
            start.ArgumentList.Add(Path.Combine(_root, _hostDll));
            if (osSearchFolder is not null)
            {
                string variable = OperatingSystem.IsWindows() ? "PATH" : "LD_LIBRARY_PATH";
                string? current = Environment.GetEnvironmentVariable(variable);
                start.Environment[variable] = string.IsNullOrEmpty(current)
                    ? osSearchFolder
                    : osSearchFolder + Path.PathSeparator + current;
            }

            using Process process = Process.Start(start)!;
            System.Threading.Tasks.Task<string> stdout = process.StandardOutput.ReadToEndAsync();
            System.Threading.Tasks.Task<string> stderr = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(TimeSpan.FromSeconds(60)))
            {
                process.Kill(entireProcessTree: true);
                Assert.Fail($"the test host did not exit within 60 s in {_root}.");
            }

            process.WaitForExit();
            return new HostRun(process.ExitCode, stdout.Result, stderr.Result, Leaf);
        }

        public void Dispose()
        {
            try
            {
                RestoreReadable();
                Directory.Delete(_root, recursive: true);
            }
            catch (UnauthorizedAccessException)
            {
                // A shim the OS still holds mapped on Windows; the temp folder is left behind.
            }
            catch (IOException)
            {
                // As above.
            }
        }
    }

    /// <summary>What one host run printed.</summary>
    private sealed class HostRun
    {
        public HostRun(int exitCode, string stdout, string stderr, string folder)
        {
            ExitCode = exitCode;
            Stdout = stdout;
            Stderr = stderr;
            Folder = folder;
            string[] lines = stdout.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            Delivered = lines.Contains("delivered");
            Modules = lines.Where(l => l.StartsWith("module ", StringComparison.Ordinal)).Select(l => l["module ".Length..]).ToArray();
            Errors = lines.Where(l => l.StartsWith("error ", StringComparison.Ordinal)).Select(l => l["error ".Length..]).ToArray();
        }

        public int ExitCode { get; }

        public bool Delivered { get; }

        public IReadOnlyList<string> Modules { get; }

        public IReadOnlyList<string> Errors { get; }

        private string Stdout { get; }

        private string Stderr { get; }

        private string Folder { get; }

        public override string ToString() =>
            $"host run in {Folder}: exit {ExitCode}\n--- stdout ---\n{Stdout}--- stderr ---\n{Stderr}";
    }
}
