// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Selection: one call, one return type, whichever kind of provider it names.
//
// ── What is deliberately NOT here ───────────────────────────────────────────
// `Register` and `SetPathResolver` are both absent, and neither is an oversight.
// Registration happens inside the shim, which links `inprocess`, `fastdds` and
// `xrce` at build time; the path-resolver seat belongs to PDA-ABI and is filled
// natively. Exposing either would let a C# process hold a registry whose
// contents differ from the one the shim resolves against - two vocabularies,
// free to drift, with the drift visible only as a selection that works in one
// process and not another.
//
// There is also NO WAY TO ASK WHAT IS AVAILABLE, for the same reason the seam
// offers none: an enumeration invites a caller to branch on it, and the property
// the round is protecting is that moving a protocol from built-in to loaded is a
// configuration edit and never a caller edit. When a selection names nothing,
// the refusal comes back from the registry naming what IS there - which is the
// answer a discovery call would have been used to get, delivered at the moment
// it is actually needed.
using System;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>Creates a provider from a selection.</summary>
public static class ProviderRegistry
{
    /// <summary>Resolve <paramref name="selector"/> and create the provider it names.</summary>
    /// <exception cref="ArgumentNullException"><paramref name="config"/> is null.</exception>
    /// <exception cref="InvalidOperationException"><paramref name="selector"/> was never parsed.</exception>
    /// <exception cref="FletcherException">
    /// The selection names nothing this build has, or the provider refused the
    /// configuration. The message names what IS available.
    /// </exception>
    public static unsafe PubSubProviderHandle Create(ProviderSelector selector, ProviderConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);

        byte[] selectorBytes = selector.Utf8;
        ReadOnlySpan<byte> document = config.Document.Span;

        FlError err = default;
        int status;
        ProviderHandle handle;

        // Both pinned for exactly the call. The seam COPIES whatever it keeps
        // (4.2), so nothing here has to outlive the return - and pinning rather
        // than marshalling is what lets the document cross verbatim, zero byte and
        // all, instead of through a string conversion that would stop at the first
        // one.
        fixed (byte* selectorData = selectorBytes)
        fixed (byte* documentData = document)
        {
            var native = new FlProviderConfig
            {
                MaxPayloadBytes = config.MaxPayloadBytes,
                DomainId = config.DomainId,
                Document = new FlStr { Data = (nint)documentData, Len = (nuint)document.Length },
            };

            var text = new FlStr { Data = (nint)selectorData, Len = (nuint)selectorBytes.Length };
            status = NativeMethods.fl_provider_create(text, in native, out handle, ref err);
        }

        Errors.ThrowIfFailed(status, ref err);
        return new PubSubProviderHandle(handle);
    }
}
