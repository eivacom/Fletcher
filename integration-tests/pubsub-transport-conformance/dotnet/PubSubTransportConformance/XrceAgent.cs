// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// A MicroXRCEAgent this suite starts, proves it owns, and stops (D-BIND-56).
//
// BIND-4's acceptance promised bucket 4 over `fastdds` AND `xrce`. Until this
// fixture existed the `xrce` half was a typed refusal with no Agent, and no C#
// had ever published or subscribed over XRCE-DDS. The Agent is the one eProsima
// ships, built by the lane from `integration-tests/cmake/MicroXrceAgent.cmake` -
// the recipe the C++ interop lane uses - and handed to this suite as
// MICRO_XRCE_AGENT_PATH (and MICRO_XRCE_AGENT_LIB_DIR for its libraries), the
// names the C++ fixture uses for the same two things.
//
// ABSENT, THE XRCE ROWS FAIL; THEY DO NOT SKIP. That is this suite's rule for the
// shim too: a transport suite that silently ran on fewer transports is the
// vacuity this round keeps catching. The other rows are unaffected - the failure
// is recorded and raised only when an XRCE provider is asked for.
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;

using Xunit;

namespace Eiva.Fletcher.TransportConformance;

/// <summary>One Agent process, owned by this suite.</summary>
internal sealed class XrceAgent : IDisposable
{
    internal const string Host = "127.0.0.1";

    /// <summary>
    /// The suite's Agent port, used by nothing else in the tree: 2018 is the C++
    /// interop fixture's, 2019 and 2119 belong to pubsub-conformance, 2118 to the
    /// interop fixture's forcing test. A developer running two suites at once must
    /// not have one certify against the other's Agent.
    /// </summary>
    internal const ushort SuitePort = 2020;

    /// <summary>The forcing tests' port, for Agents they stand up and contest on purpose.</summary>
    internal const ushort ContestedPort = 2120;

    private const int ReachableWithinMs = 15_000;

    /// <summary>
    /// Session keys are unique per client on one Agent. A base per purpose, so the
    /// probe's keys - one per attempt, several while the Agent binds - can never
    /// walk onto a row's (the C++ fixture learned that the hard way).
    /// </summary>
    private const uint RowSessionBase = 0xB1D40000u;
    private const uint ProbeSessionBase = 0xB1D50000u;
    private static int _rowSessions;
    private static int _probeSessions;

    private readonly Process _process;
    private readonly StringBuilder _log = new();

    private XrceAgent(Process process, ushort port)
    {
        _process = process;
        Port = port;
    }

    internal ushort Port { get; }

    internal int Pid => _process.Id;

    /// <summary>The document that points an XRCE provider at an Agent, with a fresh session key.</summary>
    internal static ProviderConfig ClientConfig(ushort port, uint domainId = 0) => new()
    {
        DomainId = domainId,
        Document = Encoding.UTF8.GetBytes(
            $"agent={Host}:{port}\nsession_key={RowSessionBase + (uint)(Interlocked.Increment(ref _rowSessions) & 0xFFFF)}"),
    };

    /// <summary>Spawn an Agent on <paramref name="port"/>, wait until it answers, and PROVE it is ours.</summary>
    /// <exception cref="InvalidOperationException">
    /// Any of: no Agent binary configured, it did not answer in time, it exited, or
    /// the OS does not record OUR child as the port's holder. The message says
    /// which, in words an operator can act on.
    /// </exception>
    internal static XrceAgent Start(ushort port)
    {
        string? path = Environment.GetEnvironmentVariable("MICRO_XRCE_AGENT_PATH");
        if (string.IsNullOrEmpty(path) || !File.Exists(path))
        {
            throw new InvalidOperationException(
                "MICRO_XRCE_AGENT_PATH does not name a MicroXRCEAgent executable " +
                $"(it is '{path}'). Build one with integration-tests/cmake/MicroXrceAgent.cmake, " +
                "as the lane does, and point the variable at it.");
        }

        var start = new ProcessStartInfo(path)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        start.ArgumentList.Add("udp4");
        start.ArgumentList.Add("-p");
        start.ArgumentList.Add(port.ToString(System.Globalization.CultureInfo.InvariantCulture));

        string? libDir = Environment.GetEnvironmentVariable("MICRO_XRCE_AGENT_LIB_DIR");
        if (!string.IsNullOrEmpty(libDir))
        {
            string variable = OperatingSystem.IsWindows() ? "PATH" : "LD_LIBRARY_PATH";
            string? existing = Environment.GetEnvironmentVariable(variable);
            start.Environment[variable] = string.IsNullOrEmpty(existing) ? libDir : libDir + Path.PathSeparator + existing;
        }

        Process process = Process.Start(start)
            ?? throw new InvalidOperationException($"could not start {path}");
        var agent = new XrceAgent(process, port);
        process.OutputDataReceived += (_, e) => agent.Record(e.Data);
        process.ErrorDataReceived += (_, e) => agent.Record(e.Data);
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();

        try
        {
            agent.WaitUntilReachable();
            agent.ProveOwnership();
            return agent;
        }
        catch
        {
            agent.Dispose();
            throw;
        }
    }

    private void Record(string? line)
    {
        if (line is null)
        {
            return;
        }

        lock (_log)
        {
            _log.AppendLine(line);
        }
    }

    private string Log()
    {
        lock (_log)
        {
            return _log.ToString();
        }
    }

    /// <summary>Probe by opening an XRCE session, which needs a live Agent on the port.</summary>
    private void WaitUntilReachable()
    {
        var clock = Stopwatch.StartNew();
        string last = "no attempt made";
        while (clock.ElapsedMilliseconds < ReachableWithinMs)
        {
            var probe = new ProviderConfig
            {
                Document = Encoding.UTF8.GetBytes(
                    $"agent={Host}:{Port}\nconnect_timeout_ms=500\n" +
                    $"session_key={ProbeSessionBase + (uint)(Interlocked.Increment(ref _probeSessions) & 0xFFFF)}"),
            };
            try
            {
                using PubSubProviderHandle session = ProviderRegistry.Create(ProviderSelector.Parse("xrce"), probe);
                return;
            }
            catch (FletcherException e) when (e.Status == FletcherStatus.TransportFailure)
            {
                last = e.Message;
                Thread.Sleep(100);
            }
        }

        throw new InvalidOperationException(
            $"no MicroXRCEAgent answered on {Host}:{Port} within {ReachableWithinMs} ms " +
            $"(last attempt: {last}). Agent log:\n{Log()}");
    }

    /// <summary>
    /// The OS must record THIS child as the port's holder. Asked after the probe
    /// answered, because an answer alone is exactly what a leftover Agent gives.
    /// </summary>
    private void ProveOwnership()
    {
        (PortOwnership answer, string? error) = UdpPortOwnership.Query(Port, Pid);
        switch (answer)
        {
            case PortOwnership.Ours:
                return;
            case PortOwnership.SomeoneElses:
                throw new InvalidOperationException(
                    $"UDP {Port} is held by another process, not by the MicroXRCEAgent this suite started " +
                    $"(pid {Pid}{(_process.HasExited ? ", which has exited" : string.Empty)}). A leftover Agent " +
                    $"answers the probe and would certify this run against itself: stop it and rerun. Agent log:\n{Log()}");
            case PortOwnership.Nobody:
                throw new InvalidOperationException(
                    $"nothing holds UDP {Port}, although an Agent answered on it: the MicroXRCEAgent this suite " +
                    $"started (pid {Pid}) is not the one that answered. Agent log:\n{Log()}");
            default:
                throw new InvalidOperationException(
                    $"could not ask the OS who holds UDP {Port}: {error}. This harness must not certify a " +
                    "run against an Agent it cannot prove it owns.");
        }
    }

    public void Dispose()
    {
        try
        {
            if (!_process.HasExited)
            {
                _process.Kill(entireProcessTree: true);
                _process.WaitForExit(5_000);
            }
        }
        catch (InvalidOperationException)
        {
            // Already gone between the check and the kill.
        }

        _process.Dispose();
    }
}

/// <summary>The suite's Agent, started once for every class in the collection.</summary>
/// <remarks>
/// A startup failure is RECORDED rather than thrown, so the `inprocess` and
/// `fastdds` rows still run and report; every XRCE row then fails with the reason.
/// </remarks>
public sealed class XrceAgentFixture : IDisposable
{
    private readonly XrceAgent? _agent;

    public XrceAgentFixture()
    {
        try
        {
            _agent = XrceAgent.Start(XrceAgent.SuitePort);
            StartupFailure = null;
        }
        catch (InvalidOperationException e)
        {
            StartupFailure = e.Message;
        }
    }

    /// <summary>Why the suite's Agent is not available, or null when it is.</summary>
    internal static string? StartupFailure { get; private set; } = "the XRCE Agent fixture has not run";

    /// <summary>A client document for the suite's Agent; throws the startup failure instead.</summary>
    internal static ProviderConfig ClientConfig(uint domainId = 0)
    {
        if (StartupFailure is not null)
        {
            throw new InvalidOperationException($"the suite's MicroXRCEAgent is not available: {StartupFailure}");
        }

        return XrceAgent.ClientConfig(XrceAgent.SuitePort, domainId);
    }

    public void Dispose() => _agent?.Dispose();
}

/// <summary>
/// Every class that touches an Agent. One collection, so they run one after
/// another: the forcing tests swap <see cref="UdpPortOwnership.Query"/>, which is
/// static, and must never do it while another class is proving an Agent.
/// </summary>
[CollectionDefinition(Name)]
public sealed class XrceAgentCollection : ICollectionFixture<XrceAgentFixture>
{
    public const string Name = "MicroXRCEAgent";
}
