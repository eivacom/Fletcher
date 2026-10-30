// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Process-wide diagnostics: the managed mirror of C++ `DeliveryChannel::
// AbsorbedTotal()` (D-BIND-55).
//
// Why a managed counter at all: a delivery thunk catches every managed exception
// itself (D-BIND-19 rule 5), so native's absorbed count never sees a C# handler
// fail - constraint 8. Each Subscriber keeps its own count; this is their sum.
//
// Why a process-wide number means anything: D-BIND-17 allows one copy of
// Fletcher per process, and Q11 ruled that its consumers are .NET processes
// hosting this binding only. So "every C# handler failure in this process" is
// one well-defined count, not the sum over copies that cannot see each other.
using System.Threading;

namespace Eiva.Fletcher;

/// <summary>Process-wide diagnostics.</summary>
public static class Diagnostics
{
    private static long _absorbedTotal;

    /// <summary>Handler failures absorbed by every <see cref="Subscriber"/> in this process.</summary>
    /// <remarks>
    /// The sum of every <see cref="Subscriber.AbsorbedCallbackFailures"/> there has
    /// been, including those of subscribers already disposed. Only ever increases.
    /// It counts failures of C# handlers; a C++ handler in the same process, which
    /// D-BIND-17 and Q11 rule out, would be counted by C++ instead.
    /// </remarks>
    public static ulong AbsorbedTotal => (ulong)Interlocked.Read(ref _absorbedTotal);

    internal static void CountAbsorbed() => Interlocked.Increment(ref _absorbedTotal);
}
