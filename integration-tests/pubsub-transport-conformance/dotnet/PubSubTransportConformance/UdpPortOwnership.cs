// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Who holds a UDP port - the question PDA-DEC-1H made every Agent harness ask.
//
// ── Why reachability is not enough (D-BIND-56) ──────────────────────────────
// A second MicroXRCEAgent aimed at a port a leftover already holds logs `bind
// error` and exits within tens of milliseconds, while the leftover answers the
// reachability probe in milliseconds. A harness that only asked "does an Agent
// answer?" therefore certifies a run against a FOREIGN Agent - possibly on
// another DDS domain - and passes. So the fixture asks the OS which process holds
// the port and requires that it be the child this suite spawned.
//
// This is the THIRD copy of the rule. The other two are in
// `integration-tests/fastdds-xrce-interop/tests/test_interop.cpp` and
// `integration-tests/pubsub-conformance`, each in C++; they cannot share with a
// C# harness, so the pairing that keeps copies from drifting is the same here as
// there - a negative test per refusal, asserting the same four answers:
//
//   Ours           the process we spawned holds it. The only pass.
//   SomeoneElses   another process holds it - a leftover Agent, typically.
//   Nobody         the OS records no holder at all.
//   QueryFailed    the OS could not be asked. A REFUSAL, never "unknown, carry on".
//
// IPv4 only on both platforms, as in C++: the Agent is started as `udp4`, so an
// IPv6 row on the port could not be the endpoint the suite certifies against.
// And "foreign beats ours": every socket on the port must be ours.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;

namespace Eiva.Fletcher.TransportConformance;

internal enum PortOwnership
{
    Ours,
    SomeoneElses,
    Nobody,
    QueryFailed,
}

internal static class UdpPortOwnership
{
    /// <summary>
    /// The query, reached through ONE indirection so a test can force the
    /// <see cref="PortOwnership.QueryFailed"/> arm. In C++ that arm was an
    /// earlier revision's fallback to bare liveness, which PASSED, and nothing
    /// reddened when it was re-introduced until a test could reach it.
    /// </summary>
    internal static Func<ushort, int, (PortOwnership Answer, string? Error)> Query { get; set; } = Ask;

    internal static (PortOwnership Answer, string? Error) Ask(ushort port, int ourPid)
    {
        if (OperatingSystem.IsWindows())
        {
            return AskWindows(port, ourPid);
        }

        if (OperatingSystem.IsLinux())
        {
            return AskLinux(port, ourPid);
        }

        // No fallback on a third platform: porting a query is a precondition for
        // certifying a run, exactly as the C++ copies make it a build error.
        return (PortOwnership.QueryFailed,
            $"no UDP port-ownership query exists for {RuntimeInformation.OSDescription}; " +
            "this harness must not certify a run against an Agent it cannot prove it owns");
    }

    private static (PortOwnership, string?) AskWindows(ushort port, int ourPid)
    {
        // Sized, then read - and the table can grow between the two calls, hence the retry.
        for (int attempt = 0; attempt < 4; attempt++)
        {
            int size = 0;
            uint rc = NativeMethods.GetExtendedUdpTable(IntPtr.Zero, ref size, false,
                NativeMethods.AfInet, NativeMethods.UdpTableOwnerPid, 0);
            if (rc != NativeMethods.ErrorInsufficientBuffer && rc != 0)
            {
                return (PortOwnership.QueryFailed, $"GetExtendedUdpTable failed sizing the table (error {rc})");
            }

            IntPtr buffer = Marshal.AllocHGlobal(size);
            try
            {
                rc = NativeMethods.GetExtendedUdpTable(buffer, ref size, false,
                    NativeMethods.AfInet, NativeMethods.UdpTableOwnerPid, 0);
                if (rc == NativeMethods.ErrorInsufficientBuffer)
                {
                    continue;
                }

                if (rc != 0)
                {
                    return (PortOwnership.QueryFailed, $"GetExtendedUdpTable failed reading the table (error {rc})");
                }

                // MIB_UDPTABLE_OWNER_PID: a DWORD count, then rows of
                // {DWORD localAddr, DWORD localPort, DWORD owningPid}. The port is
                // in network byte order in the low 16 bits of its DWORD.
                int count = Marshal.ReadInt32(buffer);
                bool any = false;
                for (int i = 0; i < count; i++)
                {
                    int row = 4 + (i * 12);
                    int rawPort = Marshal.ReadInt32(buffer, row + 4);
                    ushort rowPort = (ushort)(((rawPort & 0xFF) << 8) | ((rawPort >> 8) & 0xFF));
                    if (rowPort != port)
                    {
                        continue;
                    }

                    any = true;
                    if (Marshal.ReadInt32(buffer, row + 8) != ourPid)
                    {
                        return (PortOwnership.SomeoneElses, null);
                    }
                }

                return (any ? PortOwnership.Ours : PortOwnership.Nobody, null);
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        return (PortOwnership.QueryFailed,
            "GetExtendedUdpTable reported ERROR_INSUFFICIENT_BUFFER on four consecutive attempts");
    }

    private static (PortOwnership, string?) AskLinux(ushort port, int ourPid)
    {
        var portInodes = new HashSet<string>(StringComparer.Ordinal);
        string[] lines;
        try
        {
            lines = File.ReadAllLines("/proc/net/udp");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return (PortOwnership.QueryFailed, $"/proc/net/udp could not be read ({e.Message})");
        }

        // sl / local_address / rem_address / st / tx:rx / tr:tm / retrnsmt / uid /
        // timeout / inode - so `inode` is the tenth whitespace-separated token.
        for (int i = 1; i < lines.Length; i++)
        {
            string[] token = lines[i].Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (token.Length < 10)
            {
                continue;
            }

            int colon = token[1].LastIndexOf(':');
            if (colon < 0 ||
                !ushort.TryParse(token[1].AsSpan(colon + 1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out ushort rowPort) ||
                rowPort != port)
            {
                continue;
            }

            portInodes.Add(token[9]);
        }

        if (portInodes.Count == 0)
        {
            return (PortOwnership.Nobody, null);
        }

        // A pid that no longer exists holds no sockets - a real answer, and a dead
        // child cannot be the owner. A PERMISSION refusal is the one unanswerable case.
        var ourInodes = new HashSet<string>(StringComparer.Ordinal);
        string fdDir = $"/proc/{ourPid}/fd";
        try
        {
            if (Directory.Exists(fdDir))
            {
                foreach (string fd in Directory.EnumerateFileSystemEntries(fdDir))
                {
                    string? target = new FileInfo(fd).LinkTarget;
                    if (target is not null && target.StartsWith("socket:[", StringComparison.Ordinal) && target.EndsWith(']'))
                    {
                        ourInodes.Add(target[8..^1]);
                    }
                }
            }
        }
        catch (UnauthorizedAccessException)
        {
            return (PortOwnership.QueryFailed,
                $"{fdDir} could not be read (access denied), so the sockets our own child holds cannot be listed");
        }

        foreach (string inode in portInodes)
        {
            if (!ourInodes.Contains(inode))
            {
                return (PortOwnership.SomeoneElses, null);
            }
        }

        return (PortOwnership.Ours, null);
    }

    private static class NativeMethods
    {
        internal const int AfInet = 2;
        internal const int UdpTableOwnerPid = 1;
        internal const uint ErrorInsufficientBuffer = 122;

        // A system library, not a new dependency - the same one the C++ copies link.
        [DllImport("iphlpapi.dll", ExactSpelling = true)]
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        internal static extern uint GetExtendedUdpTable(
            IntPtr table, ref int size, [MarshalAs(UnmanagedType.Bool)] bool order,
            int addressFamily, int tableClass, uint reserved);
    }
}
