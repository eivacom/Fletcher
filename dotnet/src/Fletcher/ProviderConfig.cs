// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// HOW to configure it: a small typed core, plus bytes Fletcher carries and never
// reads.
//
// ── The typed core is exactly two fields, and that is a ruling ──────────────
// "Fletcher keeps exactly payload size and domain; everything protocol-specific
// lives in the document only that protocol reads" (owner ruling 2026-09-02). It
// is append-only: a later field never changes Create's signature. Widening it
// because one protocol wants a setting typed is a stop-and-ask, not an edit - so
// if you are here to add a third property, that is the conversation to have
// first.
using System;

namespace Eiva.Fletcher;

/// <summary>What a provider is created with.</summary>
public sealed class ProviderConfig
{
    /// <summary>The largest encoded sample the transport should accept, in bytes.</summary>
    /// <remarks>
    /// <para>
    /// ZERO MEANS UNSET, and the provider's own default applies. Fletcher does not
    /// know any provider's valid bounds or its default, so it cannot demand a value
    /// it could check; "unset" is safe to spell as 0 because
    /// <see cref="PayloadBound.IsValid"/> is false for 0 everywhere, so no provider
    /// can mistake it for a real bound.
    /// </para>
    /// <para>
    /// Not validated here. A caller who wants the refusal at the line that parsed
    /// their configuration calls <see cref="PayloadBound.IsValid"/> there; a value
    /// this type rejected would be one the provider might have accepted, and
    /// Fletcher does not know which.
    /// </para>
    /// </remarks>
    public uint MaxPayloadBytes { get; init; }

    /// <summary>The DDS domain, for providers that have one.</summary>
    public uint DomainId { get; init; }

    /// <summary>
    /// Protocol-specific configuration, carried verbatim to the provider.
    /// </summary>
    /// <remarks>
    /// BYTES, NOT A STRING, and Fletcher parses none of it - explicitly a non-goal
    /// (seam 4.2). The length is authoritative, so a document containing a zero
    /// byte crosses whole rather than being truncated at the first one. That is
    /// also why this is <see cref="ReadOnlyMemory{T}"/> rather than
    /// <c>string</c>: a string would force an encoding decision onto a payload
    /// whose encoding is the receiving protocol's business.
    /// </remarks>
    public ReadOnlyMemory<byte> Document { get; init; }
}
