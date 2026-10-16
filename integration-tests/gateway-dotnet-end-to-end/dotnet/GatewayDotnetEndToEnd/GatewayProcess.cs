// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The real gateway exe, spawned for the tests and stopped after them.
//
// As integration-tests/gateway-end-to-end's spawnGateway does it: --port, --bind-address
// 127.0.0.1, --provider (and --domain-id for fastdds); ready when it prints "READY <port>";
// stopped cooperatively by writing "stop" to its stdin, killed if it does not exit in 5 s.
// The binary is GATEWAY_BIN, or the one `conan build .` left under ../../build.
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

using Xunit;

namespace Eiva.Fletcher.GatewayDotnetEndToEnd;

/// <summary>One provider's gateway: its process and the address a client connects to.</summary>
public abstract class GatewayProcess : IAsyncLifetime
{
    private Process? _process;

    protected GatewayProcess(string provider, int port, string? domainId)
    {
        Provider = provider;
        Port = port;
        DomainId = domainId;
    }

    public string Provider { get; }

    public int Port { get; }

    public string? DomainId { get; }

    public Uri Uri => new($"ws://127.0.0.1:{Port}");

    /// <summary>How long a published row may take to come back: Fast DDS goes through discovery.</summary>
    public abstract TimeSpan RoundTrip { get; }

    public async Task InitializeAsync()
    {
        string args = $"--port {Port} --bind-address 127.0.0.1 --provider {Provider}" + (DomainId is null ? string.Empty : $" --domain-id {DomainId}");
        var start = new ProcessStartInfo(FindGatewayBinary(), args)
        {
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        _process = Process.Start(start) ?? throw new InvalidOperationException("the gateway did not start");
        _ = _process.StandardError.ReadToEndAsync();

        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        while (true)
        {
            string? line = await _process.StandardOutput.ReadLineAsync(deadline.Token);
            if (line is null)
            {
                throw new InvalidOperationException($"the gateway ({Provider}) exited before READY (code {(_process.HasExited ? _process.ExitCode : -1)})");
            }

            if (line.StartsWith("READY ", StringComparison.Ordinal))
            {
                break;
            }
        }

        // Keep draining stdout so the gateway never blocks on a full pipe.
        _ = _process.StandardOutput.ReadToEndAsync();
    }

    public async Task DisposeAsync()
    {
        if (_process is null)
        {
            return;
        }

        try
        {
            await _process.StandardInput.WriteLineAsync("stop");
            await _process.StandardInput.FlushAsync();
            using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            await _process.WaitForExitAsync(deadline.Token);
        }
        catch (Exception e) when (e is OperationCanceledException or IOException)
        {
            _process.Kill(entireProcessTree: true);
        }
        finally
        {
            _process.Dispose();
        }
    }

    private static string FindGatewayBinary()
    {
        string? fromEnv = Environment.GetEnvironmentVariable("GATEWAY_BIN");
        if (!string.IsNullOrEmpty(fromEnv))
        {
            return fromEnv;
        }

        string name = OperatingSystem.IsWindows() ? "gateway.exe" : "gateway";
        string build = Path.GetFullPath(Path.Combine(ProjectDir(), "..", "..", "build"));
        string? found = Directory.Exists(build)
            ? Directory.EnumerateFiles(build, name, SearchOption.AllDirectories).FirstOrDefault()
            : null;
        return found ?? throw new FileNotFoundException(
            $"gateway binary ({name}) not found under {build}: run `conan build .` in integration-tests/gateway-dotnet-end-to-end, or set GATEWAY_BIN");
    }

    // The project directory, from the assembly's location: bin/<config>/<tfm>/ under it.
    private static string ProjectDir() =>
        Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", ".."));
}

/// <summary>The gateway over the in-process provider: a loopback inside the gateway.</summary>
public sealed class InProcessGateway : GatewayProcess
{
    public InProcessGateway()
        : base("inprocess", 19191, null)
    {
    }

    public override TimeSpan RoundTrip => TimeSpan.FromSeconds(5);
}

/// <summary>The gateway over Fast DDS, on its own port and an isolated domain.</summary>
public sealed class FastDdsGateway : GatewayProcess
{
    public FastDdsGateway()
        : base("fastdds", 19194, Environment.GetEnvironmentVariable("DDS_DOMAIN_ID") ?? "152")
    {
    }

    public override TimeSpan RoundTrip => TimeSpan.FromSeconds(15);
}
