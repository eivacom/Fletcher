// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The payload bound, exposed so a caller can check one WHERE IT IS WRITTEN.
//
// The seam takes a bound as a plain number on ProviderConfig and each provider
// decides what it can honour, so nothing before Create can tell a caller that
// 5000 is not a bound. That is the whole reason this type is public: a
// configuration reader validating its own input gets an answer at the line that
// parsed it, rather than at a construction three layers away that reports the
// same fact with none of the context.
//
// The numbers are the seam's (pubsub/payload_bound.hpp) and are reproduced, not
// re-derived - which is a second copy, and the risk that comes with one. No
// entry point exports these constants, so nothing here can read the native
// values at run time and the drift cannot be closed the way the status taxonomy
// closes it. What stands in for that: the test asserts each number as a LITERAL
// and names the header it came from, so a change on either side fails a test
// that says where to look. Stated rather than implied, because a reader is
// entitled to know this pair is checked by convention and not by construction.
namespace Eiva.Fletcher;

/// <summary>What numbers can bound a Fletcher payload.</summary>
public static class PayloadBound
{
    /// <summary>The sample's own 4-byte length plus the 4-byte CDR encapsulation header.</summary>
    /// <remarks>
    /// A bound PLUS these is the size Fast DDS is told the type has, and it reports
    /// that in a uint32 - which is what puts the ceiling below <c>uint.MaxValue</c>
    /// rather than at it.
    /// </remarks>
    public const uint FramingBytes = 8;

    /// <summary>The smallest envelope that can exist.</summary>
    public const uint Min = 4;

    /// <summary>Where a sample's size stops fitting the uint32 it is reported in.</summary>
    public const uint Max = (uint.MaxValue - FramingBytes) & ~3u;

    /// <summary>Whether <paramref name="bytes"/> can bound a payload.</summary>
    /// <remarks>
    /// The multiple-of-4 rule is not arbitrary and is not ours: Fast DDS delivers
    /// zero-copy only for a PLAIN type, one carrying no padding, and a sample is a
    /// 4-byte length followed by the body - so it is padding-free exactly while the
    /// body is 4-aligned. Nothing else about the number matters to the transport.
    ///
    /// <c>IsValid(0)</c> is false, which is what lets <see cref="ProviderConfig"/>
    /// spell "unset" as 0 without any provider mistaking it for a real bound.
    /// </remarks>
    public static bool IsValid(uint bytes) => bytes >= Min && bytes <= Max && bytes % 4 == 0;
}
