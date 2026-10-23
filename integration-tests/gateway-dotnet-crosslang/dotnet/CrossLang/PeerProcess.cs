// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// A child process of the lane that speaks the peers' line protocol: it prints READY once set
// up, then lines the test reads (<topic> RECV ..., PUBLISHED ..., ERROR ...), takes commands on
// stdin (`publish <topic>`, `probe <topic>`, `stop`), and stops on `stop`, or is killed if it
// does not exit in 5 s. The gateway is one too: it prints `READY <port>` and stops on `stop`.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace Eiva.Fletcher.CrossLang;

public sealed class PeerProcess : IAsyncDisposable
{
    private readonly Process _process;
    private readonly ConcurrentQueue<string> _lines = new();
    private readonly ConcurrentQueue<string> _errors = new();

    private PeerProcess(string label, Process process)
    {
        Label = label;
        _process = process;
    }

    public string Label { get; }

    /// <summary>True once the process has exited: a crash, not a lost row.</summary>
    public bool HasExited => _process.HasExited;

    /// <summary>Every stdout line after READY, in order.</summary>
    public IReadOnlyList<string> Lines => _lines.ToArray();

    /// <summary>The last stderr lines, for a failure message.</summary>
    public string StandardErrorTail => string.Join(" | ", _errors.TakeLast(10));

    public static async Task<PeerProcess> StartAsync(string label, string fileName, IEnumerable<string> arguments, string? workingDirectory = null)
    {
        var start = new ProcessStartInfo(fileName)
        {
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        foreach (string argument in arguments)
        {
            start.ArgumentList.Add(argument);
        }

        if (workingDirectory is not null)
        {
            start.WorkingDirectory = workingDirectory;
        }

        Process process = Process.Start(start) ?? throw new InvalidOperationException($"{label} did not start ({fileName})");
        var peer = new PeerProcess(label, process);
        _ = Task.Run(async () =>
        {
            while (await process.StandardError.ReadLineAsync() is { } line)
            {
                peer._errors.Enqueue(line);
            }
        });

        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(60));
        try
        {
            while (true)
            {
                string? line = await process.StandardOutput.ReadLineAsync(deadline.Token);
                if (line is null)
                {
                    await process.WaitForExitAsync(CancellationToken.None);
                    throw new InvalidOperationException($"{label} exited before READY (code {process.ExitCode}); stderr: {peer.StandardErrorTail}");
                }

                if (line.StartsWith("READY", StringComparison.Ordinal))
                {
                    break;
                }
            }
        }
        catch (OperationCanceledException)
        {
            process.Kill(entireProcessTree: true);
            throw new TimeoutException($"{label} did not print READY within 60 s; stderr: {peer.StandardErrorTail}");
        }

        _ = Task.Run(async () =>
        {
            while (await process.StandardOutput.ReadLineAsync() is { } line)
            {
                peer._lines.Enqueue(line);
            }
        });
        return peer;
    }

    public async Task SendAsync(string command)
    {
        await _process.StandardInput.WriteLineAsync(command);
        await _process.StandardInput.FlushAsync();
    }

    /// <summary>Waits until <paramref name="done"/> holds over the lines read so far, or the time is up.</summary>
    public async Task<bool> WaitForAsync(Func<IReadOnlyList<string>, bool> done, TimeSpan timeout)
    {
        DateTime deadline = DateTime.UtcNow + timeout;
        while (!done(Lines))
        {
            if (DateTime.UtcNow >= deadline || _process.HasExited)
            {
                return done(Lines);
            }

            await Task.Delay(50);
        }

        return true;
    }

    public async ValueTask DisposeAsync()
    {
        try
        {
            if (!_process.HasExited)
            {
                await SendAsync("stop");
                using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                await _process.WaitForExitAsync(deadline.Token);
            }
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
}
