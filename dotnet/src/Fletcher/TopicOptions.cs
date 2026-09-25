// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The seam's per-topic options (D-BIND-57), which #128 added after the C# surface
// was frozen and which the ABI did not carry until 0.6.
//
// NOT VALIDATED HERE, on purpose and for ProviderConfig's reason: Fletcher does
// not know which profiles a provider's document defines, or which bounds it
// accepts. The seam checks every field and answers with its own status, so a
// caller sees exactly what a C++ caller would - FletcherStatus.InvalidArgument
// for an unknown profile, a bound on a subscription, or a re-declaration that
// changes a stored field; FletcherStatus.NotSupported from a provider with no
// notion of the field.
namespace Eiva.Fletcher;

/// <summary>Per-topic options for <see cref="Publisher.CreateTopic(TopicPath, Apache.Arrow.Schema, TopicOptions?)"/>
/// and <see cref="Subscriber.Subscribe(TopicPath, RowHandler, TopicOptions?)"/>.</summary>
/// <remarks>
/// An empty value - no profile, no bound - means the provider's defaults and is
/// never refused. Options are checked FIELD BY FIELD against a topic already
/// declared or subscribed: a later call may repeat or omit a field, never change
/// one, and a non-empty field against an empty stored one is a conflict too.
/// </remarks>
public sealed class TopicOptions
{
    /// <summary>A profile name the provider resolves from its document; null or empty for none.</summary>
    /// <remarks>
    /// Opaque to Fletcher. For Fast DDS it names a <c>&lt;data_writer&gt;</c> or
    /// <c>&lt;data_reader&gt;</c> profile in the provider's XML document; the
    /// XRCE and in-process providers take none.
    /// </remarks>
    public string? Profile { get; init; }

    /// <summary>A payload bound for this topic's PUBLISHER only; 0 for the provider's own.</summary>
    /// <remarks>
    /// A subscription carries no bound - it follows what the publisher announces -
    /// so a non-zero value on <c>Subscribe</c> is refused.
    /// </remarks>
    public uint MaxPayloadBytes { get; init; }

    /// <summary>Whether this is the provider-defaults value.</summary>
    public bool IsEmpty => string.IsNullOrEmpty(Profile) && MaxPayloadBytes == 0;
}
