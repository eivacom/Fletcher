// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-4b: the attachments write end.
//
// These rows read the sealed set back through the interop accessors rather than
// asserting a COUNT. That distinction is the point: a builder that dropped every
// value and kept the keys, or that handed the seam a truncated span, would pass
// any count-based test and fail only once a subscriber existed to notice - two
// slices later, with the cause three commits back.
using System;
using System.Text;

using Eiva.Fletcher;
using Eiva.Fletcher.Interop;

using Xunit;

namespace Eiva.Fletcher.Tests;

public class AttachmentsBuilderTests
{
    /// <summary>The bytes of entry <paramref name="index"/>, copied out of the sealed set.</summary>
    private static unsafe (byte[] Key, byte[] Value) EntryAt(AttachmentsHandle set, int index)
    {
        FlStr key = NativeMethods.fl_attachments_key_at(set, (nuint)index);
        FlBlob value = NativeMethods.fl_attachments_value_at(set, (nuint)index);

        var keyBytes = new ReadOnlySpan<byte>((void*)key.Data, (int)key.Len).ToArray();
        byte[] valueBytes = value.Data == 0
            ? []
            : new ReadOnlySpan<byte>((void*)value.Data, (int)value.Size).ToArray();

        return (keyBytes, valueBytes);
    }

    [Fact]
    public void TheBytesGivenAreTheBytesSealed()
    {
        using var builder = new AttachmentsBuilder();
        builder.Set("origin", [0xDE, 0xAD, 0xBE, 0xEF]);

        AttachmentsHandle set = builder.Build();

        Assert.Equal(1, (int)NativeMethods.fl_attachments_size(set));
        (byte[] key, byte[] value) = EntryAt(set, 0);
        Assert.Equal(Encoding.UTF8.GetBytes("origin"), key);
        Assert.Equal(new byte[] { 0xDE, 0xAD, 0xBE, 0xEF }, value);
    }

    [Fact]
    public void TheSetIsOrderedByKeyBYTESWhateverOrderItWasBuiltIn()
    {
        // The seam inserts in key order, so the sealed set is sorted however the
        // caller filled it. Asserted here because a binding that preserved
        // INSERTION order would look right in every single-entry test and disagree
        // with the wire the moment there were two.
        using var builder = new AttachmentsBuilder();
        builder.Set("zulu", [1]);
        builder.Set("alpha", [2]);
        builder.Set("mike", [3]);

        AttachmentsHandle set = builder.Build();

        Assert.Equal(3, (int)NativeMethods.fl_attachments_size(set));
        Assert.Equal(Encoding.UTF8.GetBytes("alpha"), EntryAt(set, 0).Key);
        Assert.Equal(Encoding.UTF8.GetBytes("mike"), EntryAt(set, 1).Key);
        Assert.Equal(Encoding.UTF8.GetBytes("zulu"), EntryAt(set, 2).Key);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void KeysWhoseUtf16AndUtf8OrdersDisagreeAreOrderedByUtf8Bytes(bool emojiFirst)
    {
        // THE TWO MAPPING TRAPS BIND-4's BULLET NAMES (conformance review,
        // bullet 11). The test above uses ASCII keys, where every ordering agrees,
        // so it could not tell a byte-ordered set from one ordered by .NET string
        // comparison. These two keys disagree:
        //
        //   U+E000   UTF-16 E000        UTF-8 EE 80 80
        //   U+1F600  UTF-16 D83D DE00   UTF-8 F0 9F 98 80
        //
        // By UTF-16 code units the supplementary-plane key sorts FIRST (its high
        // surrogate D83D is below E000); by UTF-8 bytes it sorts SECOND (F0 is above
        // EE). The wire orders by bytes, so the sealed set must too - whichever
        // order the caller inserted them in, which is why this is a theory.
        string privateUse = char.ConvertFromUtf32(0xE000);
        string supplementary = char.ConvertFromUtf32(0x1F600);

        using var builder = new AttachmentsBuilder();
        if (emojiFirst)
        {
            builder.Set(supplementary, [1]);
            builder.Set(privateUse, [2]);
        }
        else
        {
            builder.Set(privateUse, [2]);
            builder.Set(supplementary, [1]);
        }

        AttachmentsHandle set = builder.Build();

        Assert.Equal(2, (int)NativeMethods.fl_attachments_size(set));
        Assert.Equal(new byte[] { 0xEE, 0x80, 0x80 }, EntryAt(set, 0).Key);
        Assert.Equal(new byte[] { 0xF0, 0x9F, 0x98, 0x80 }, EntryAt(set, 1).Key);

        // And the precondition that makes this a trap rather than a coincidence:
        // .NET's own ordinal order is the OTHER way round.
        Assert.True(string.CompareOrdinal(supplementary, privateUse) < 0);
    }

    [Fact]
    public void SettingAKeyTwiceReplacesItRatherThanDuplicatingIt()
    {
        using var builder = new AttachmentsBuilder();
        builder.Set("k", [1]);
        builder.Set("k", [2, 3]);

        Assert.Equal(1, builder.Count);

        AttachmentsHandle set = builder.Build();
        Assert.Equal(1, (int)NativeMethods.fl_attachments_size(set));
        Assert.Equal(new byte[] { 2, 3 }, EntryAt(set, 0).Value);
    }

    [Fact]
    public void TheValueIsCopiedSoALaterMutationCannotChangeWhatWasSet()
    {
        var caller = new byte[] { 1, 2, 3 };

        using var builder = new AttachmentsBuilder();
        builder.Set("k", caller);

        caller[0] = 99;

        AttachmentsHandle set = builder.Build();
        Assert.Equal(new byte[] { 1, 2, 3 }, EntryAt(set, 0).Value);
    }

    [Fact]
    public void SealingTwiceWithoutAMutationReusesTheSameSet()
    {
        // THE REASON THIS TYPE OWNS ITS ENTRIES. `fl_attachments_builder_build`
        // MOVES the pending entries out and leaves the native builder empty, so a
        // publish that sealed a caller's builder per row would attach them to row 0
        // and nothing after - silently, because an empty set is legal to publish.
        // Caching makes N publishes cost one seal AND makes the second seal
        // observably the same object rather than an empty one.
        using var builder = new AttachmentsBuilder();
        builder.Set("k", [7]);

        AttachmentsHandle first = builder.Build();
        AttachmentsHandle second = builder.Build();

        Assert.Same(first, second);
        Assert.Equal(1, (int)NativeMethods.fl_attachments_size(second));
        Assert.Equal(new byte[] { 7 }, EntryAt(second, 0).Value);
    }

    [Fact]
    public void AMutationAfterSealingIsPickedUpRatherThanIgnored()
    {
        // The other half of the cache: correct reuse must not become a stale read.
        using var builder = new AttachmentsBuilder();
        builder.Set("k", [7]);
        AttachmentsHandle first = builder.Build();

        builder.Set("k2", [8]);
        AttachmentsHandle second = builder.Build();

        Assert.NotSame(first, second);
        Assert.Equal(2, (int)NativeMethods.fl_attachments_size(second));
    }

    [Fact]
    public void ClearingDropsEverythingAndIsVisibleInTheNextSeal()
    {
        using var builder = new AttachmentsBuilder();
        builder.Set("k", [7]);
        builder.Build();

        builder.Clear();

        Assert.Equal(0, builder.Count);
        Assert.Equal(0, (int)NativeMethods.fl_attachments_size(builder.Build()));
    }

    [Fact]
    public void AnEmptySetIsLegalAndSealsToNothing()
    {
        using var builder = new AttachmentsBuilder();

        Assert.Equal(0, builder.Count);
        Assert.Equal(0, (int)NativeMethods.fl_attachments_size(builder.Build()));
    }

    [Fact]
    public void AnEmptyValueIsKeptAsAnEntryWithNoBytes()
    {
        // An entry with an empty value is a REAL entry - the key is the signal -
        // and it normalises to the empty blob, which has no owner. Worth its own
        // row because the create path special-cases size 0.
        using var builder = new AttachmentsBuilder();
        builder.Set("flag", []);

        AttachmentsHandle set = builder.Build();

        Assert.Equal(1, (int)NativeMethods.fl_attachments_size(set));
        (byte[] key, byte[] value) = EntryAt(set, 0);
        Assert.Equal(Encoding.UTF8.GetBytes("flag"), key);
        Assert.Empty(value);
    }

    [Fact]
    public void AKeyThatIsEmptyOrCarriesAZeroByteIsRefused()
    {
        using var builder = new AttachmentsBuilder();

        Assert.Throws<ArgumentException>(() => builder.Set([], [1]));
        Assert.Throws<ArgumentException>(() => builder.Set("a\0b"u8.ToArray(), [1]));

        // And the refusal left nothing behind.
        Assert.Equal(0, builder.Count);
    }

    [Fact]
    public void AKeyIsBytesSoANonAsciiOneRoundTripsAsUtf8()
    {
        using var builder = new AttachmentsBuilder();
        builder.Set("ä", [1]);

        AttachmentsHandle set = builder.Build();

        // Two bytes, not one char. The set stores what was handed to it.
        Assert.Equal(Encoding.UTF8.GetBytes("ä"), EntryAt(set, 0).Key);
        Assert.Equal(2, EntryAt(set, 0).Key.Length);
    }

    [Fact]
    public void DisposingTwiceIsSafeAndUsingADisposedBuilderIsRefused()
    {
        var builder = new AttachmentsBuilder();
        builder.Set("k", [1]);
        builder.Build();

        builder.Dispose();
        builder.Dispose();

        Assert.Throws<ObjectDisposedException>(() => builder.Set("k", [1]));
        Assert.Throws<ObjectDisposedException>(() => builder.Clear());
    }
}
