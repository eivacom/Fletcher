// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// WHAT to select: one string, exactly as an operator wrote it.
//
// ── The classification rule, reproduced because it is NORMATIVE ─────────────
// A NAME is a non-empty string of [A-Za-z0-9_-] and nothing else. Every other
// non-empty string is a PATH. No trimming, no case folding, no normalisation,
// and the rule DOES NOT CONSULT THE REGISTRY - so a given string means the same
// thing in every build, whether or not the built-in it might name is linked.
//
// The rule is total and disjoint, which is what lets one configuration setting
// carry both kinds. Every spelling of a shared library on every target - `x.so`,
// `x.dll`, `./x`, `C:\d\x.dll`, `\\host\share\x.dll` - carries a dot, a slash, a
// backslash or a colon, so it is a path; a plain word like `fastdds` is a name.
// A relative file with no dot and no separator (`myDriver`) is the one
// misclassification, and it is loud: refused as an unknown name, and the
// operator's fix is `./myDriver`.
//
// ── Why the kind is not public ──────────────────────────────────────────────
// The seam offers no way to ask whether a selection is a built-in or a loaded
// driver: no kind accessor, no second creation call, one return type either way.
// What locked decision 3 guarantees is the property with operational
// consequences - moving a protocol from built-in to loaded is a CONFIGURATION
// edit, never a caller edit - and an `IsName` on the public surface is an
// invitation to write the caller edit. It is internal here for the same reason
// C++ keeps `is_name_` private with only the registry as a friend.
using System;
using System.Text;

namespace Eiva.Fletcher;

/// <summary>A parsed provider selection: a built-in's name, or a driver's path.</summary>
public readonly struct ProviderSelector : IEquatable<ProviderSelector>
{
    private readonly string? _text;
    private readonly byte[]? _utf8;
    private readonly bool _isName;

    private ProviderSelector(string text, byte[] utf8, bool isName)
    {
        _text = text;
        _utf8 = utf8;
        _isName = isName;
    }

    /// <summary>Classify a configuration string. The only way to make a selector.</summary>
    /// <exception cref="ArgumentException">
    /// The string is empty, contains an embedded NUL, or is not encodable as UTF-8.
    /// </exception>
    /// <remarks>
    /// It never fails for any other reason. A string that names nothing this build
    /// has is a SELECTION that fails later, at
    /// <see cref="ProviderRegistry.Create"/>, where the registry can say what IS
    /// available - a much more useful refusal than one raised here could be.
    ///
    /// THE EMBEDDED NUL IS REFUSED FOR A SECURITY REASON, not for tidiness. A
    /// length-carrying binding could otherwise hand the seam
    /// <c>"fastdds\0/../evil.so"</c>, which classifies as a path and would reach a
    /// future loader's <c>dlopen(path.c_str())</c> truncated - opening a different
    /// library with no signal.
    /// </remarks>
    public static ProviderSelector Parse(string text)
    {
        ArgumentNullException.ThrowIfNull(text);

        if (text.Length == 0)
        {
            throw new ArgumentException("a provider selector cannot be the empty string", nameof(text));
        }

        if (text.Contains('\0'))
        {
            throw new ArgumentException(
                "a provider selector cannot contain a zero byte: it would reach a driver loader truncated, " +
                "opening a different library with no signal",
                nameof(text));
        }

        byte[] utf8;
        try
        {
            utf8 = StrictUtf8.GetBytes(text);
        }
        catch (EncoderFallbackException e)
        {
            throw new ArgumentException(
                $"a provider selector must be encodable as UTF-8: {e.Message}", nameof(text), e);
        }

        return new ProviderSelector(text, utf8, IsNameText(text));
    }

    private static readonly UTF8Encoding StrictUtf8 = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

    /// <summary>The rule itself, over the caller's characters.</summary>
    /// <remarks>
    /// Written over chars rather than over the encoded bytes because the character
    /// class is ASCII: every byte of a multi-byte UTF-8 sequence is >= 0x80 and so
    /// outside [A-Za-z0-9_-] either way, so the two readings agree and the char
    /// form is the one a reader can check against the spec sentence.
    /// </remarks>
    private static bool IsNameText(string text)
    {
        foreach (char c in text)
        {
            bool allowed = (c >= 'A' && c <= 'Z')
                || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9')
                || c == '_'
                || c == '-';
            if (!allowed)
            {
                return false;
            }
        }

        return true;
    }

    /// <summary>The string exactly as it was written.</summary>
    public string Text => _text ?? throw new InvalidOperationException(
        "this ProviderSelector was never parsed: use ProviderSelector.Parse(...) rather than default(ProviderSelector)");

    /// <summary>Whether this selects a built-in by name. Internal: see the file header.</summary>
    internal bool IsName => _isName;

    /// <summary>The selector's UTF-8, encoded once at parse.</summary>
    internal byte[] Utf8 => _utf8 ?? throw new InvalidOperationException(
        "this ProviderSelector was never parsed: use ProviderSelector.Parse(...) rather than default(ProviderSelector)");

    /// <inheritdoc/>
    public bool Equals(ProviderSelector other) => string.Equals(_text, other._text, StringComparison.Ordinal);

    /// <inheritdoc/>
    public override bool Equals(object? obj) => obj is ProviderSelector other && Equals(other);

    /// <inheritdoc/>
    public override int GetHashCode() => _text?.GetHashCode(StringComparison.Ordinal) ?? 0;

    /// <inheritdoc/>
    public override string ToString() => _text ?? "<default>";

    /// <summary>Whether two selectors are the same string.</summary>
    public static bool operator ==(ProviderSelector left, ProviderSelector right) => left.Equals(right);

    /// <summary>Whether two selectors are different strings.</summary>
    public static bool operator !=(ProviderSelector left, ProviderSelector right) => !left.Equals(right);
}
