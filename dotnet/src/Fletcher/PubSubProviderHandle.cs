// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// What a caller holds after selecting a transport: an opaque handle, and
// deliberately nothing else (D-BIND-24).
//
// ── Why there is no PubSubProvider class ────────────────────────────────────
// The C++ PubSubProvider is a twelve-method interface, and a binding that
// mirrored it would publish a second way to reach the transport that bypasses
// Publisher and Subscriber - the two tiers the seam's guarantees are actually
// stated about. What a C# caller needs from a provider is to have MADE one and
// to keep it alive; every operation on it belongs to a tier above.
//
// ── The lifetime this type is here to get right ─────────────────────────────
// The seam requires a provider to outlive everything built on it, and .NET does
// not order finalization: if a publisher and its provider become garbage in the
// same collection, both are finalized and the order is unspecified. That is
// handled one level down, in `PublisherHandle.Create`, through SafeHandle's own
// reference count - so what this type owns is the caller's EXPLICIT release, and
// disposing it while a publisher still holds a reference is survivable rather
// than fatal.
using System;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>A live provider. Opaque by design; the tiers above it do the work.</summary>
public sealed class PubSubProviderHandle : IDisposable
{
    private readonly ProviderHandle _handle;

    internal PubSubProviderHandle(ProviderHandle handle) => _handle = handle;

    /// <summary>The interop handle, for the tiers built on this provider.</summary>
    internal ProviderHandle Handle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
            return _handle;
        }
    }

    /// <summary>Release the provider.</summary>
    /// <remarks>
    /// Safe to call more than once. A publisher created over this provider has
    /// taken its own reference, so this does not pull the transport out from under
    /// one that is still running - the native object goes when the last holder
    /// lets go, not when this line runs.
    /// </remarks>
    public void Dispose() => _handle.Dispose();
}
