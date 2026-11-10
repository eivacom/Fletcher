// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-4b's vocabulary: TopicPath, ProviderSelector, ProviderConfig,
// PayloadBound and the registry that consumes them.
//
// ── What these tests are actually for ───────────────────────────────────────
// Most of this surface is a SECOND implementation of a rule the seam already
// enforces, which the round permits here for one reason (D-BIND-20): a refusal
// raised in managed code carries the caller's own stack. The risk that buys is
// drift, so the tests below are written as agreement checks wherever they can
// be - the refusals are asserted against the same inputs the native suite
// refuses, and the one rule that CANNOT be checked against native at run time
// (the payload bound's constants) says so out loud rather than looking checked.
using System;
using System.Text;

using Eiva.Fletcher;

using Xunit;

namespace Eiva.Fletcher.Tests;

public class TopicPathTests
{
    [Fact]
    public void TheJoinedNameIsTheIdentityEveryProviderSees()
    {
        TopicPath topic = TopicPath.Of("bind", "rows");

        Assert.Equal("bind/rows", topic.ToKey());
        Assert.Equal(new[] { "bind", "rows" }, topic.Segments);
        Assert.Equal("bind/rows", topic.ToString());
    }

    [Fact]
    public void AllSixRulesAreRefusedWithTheirOwnReason()
    {
        // 1 - the empty LIST. There is no default topic.
        Assert.Throws<ArgumentException>(() => TopicPath.Of());

        // 4 - an empty SEGMENT, which is rule 1 one level down: {""} names what
        // the empty list would, and {"a",""} names "a/".
        Assert.Throws<ArgumentException>(() => TopicPath.Of(""));
        Assert.Throws<ArgumentException>(() => TopicPath.Of("a", ""));

        // 5 - the "__" PREFIX, not the literal "__schema".
        Assert.Throws<ArgumentException>(() => TopicPath.Of("__schema"));
        Assert.Throws<ArgumentException>(() => TopicPath.Of("a", "__anything"));

        // 3 - the separator, which would name a different segment list.
        Assert.Throws<ArgumentException>(() => TopicPath.Of("a/b"));

        // 2 - a zero byte, which truncates the name on the wire.
        Assert.Throws<ArgumentException>(() => TopicPath.Of("a\0b"));
    }

    [Fact]
    public void ASingleUnderscoreIsFineAndOnlyTheDoubledPrefixIsReserved()
    {
        // The rule is a PREFIX of two, so neither of these is reserved. Worth
        // pinning: a naive implementation that checked `Contains("__")` would
        // refuse the second, and a caller with an underscore-separated naming
        // convention would find half their topics rejected for no stated reason.
        Assert.Equal("_private", TopicPath.Of("_private").ToKey());
        Assert.Equal("a__b", TopicPath.Of("a__b").ToKey());
    }

    [Fact]
    public void TheLengthCapIsMeasuredInUtf8BytesAndNotInCharacters()
    {
        // THE MANAGED-SPECIFIC RULE, and the reason this test exists at all. A
        // .NET string is UTF-16: 200 two-byte characters are 200 chars and 400
        // encoded bytes. Measuring the cap in chars would accept this name and the
        // transport would truncate it silently - exactly the failure rule 6 exists
        // to prevent, reintroduced by the binding rather than by the seam.
        string wide = new string('\u00e4', 200);  // 'a' with diaeresis: 2 bytes each
        Assert.Equal(200, wide.Length);
        Assert.Equal(400, Encoding.UTF8.GetByteCount(wide));

        ArgumentException refused = Assert.Throws<ArgumentException>(() => TopicPath.Of(wide));
        Assert.Contains("400 bytes", refused.Message, StringComparison.Ordinal);
        Assert.Contains("246", refused.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void TheCapIsExactlyAtTwoHundredAndFortySixJoinedBytes()
    {
        // The separators count, which is where an off-by-one lives: two segments
        // of 123 bytes join to 247, not 246.
        string a = new string('a', 123);
        Assert.Throws<ArgumentException>(() => TopicPath.Of(a, a));

        TopicPath atTheLimit = TopicPath.Of(new string('a', 123), new string('b', 122));
        Assert.Equal(246, Encoding.UTF8.GetByteCount(atTheLimit.ToKey()));
    }

    [Fact]
    public void AStringThatCannotBeEncodedAsUtf8IsRefusedRatherThanSubstituted()
    {
        // A lone high surrogate is a legal .NET string and an illegal UTF-8
        // sequence. The DEFAULT encoder replaces it with U+FFFD, which would make
        // this a different topic - one the caller cannot type and cannot subscribe
        // to. Refusing is the only answer that does not silently rename it.
        string loneSurrogate = "a\ud800b";

        ArgumentException refused = Assert.Throws<ArgumentException>(() => TopicPath.Of(loneSurrogate));
        Assert.Contains("UTF-8", refused.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ADefaultedTopicSaysSoRatherThanNamingTheEmptyString()
    {
        // A struct can always be defaulted and C# offers no way to refuse it, so
        // the hole the private constructor cannot close is closed at every use. The
        // failure mode this prevents is a defaulted TopicPath reaching the seam as
        // the empty list and being refused there with a message about segments the
        // caller never wrote.
        TopicPath defaulted = default;

        Assert.False(defaulted.IsConstructed);
        Assert.Empty(defaulted.Segments);
        Assert.Throws<InvalidOperationException>(() => defaulted.ToKey());
        Assert.Throws<InvalidOperationException>(() => defaulted.Utf8Segments);
    }

    [Fact]
    public void TheSegmentsAreCopiedSoALaterMutationCannotRenameTheTopic()
    {
        var caller = new[] { "bind", "rows" };
        TopicPath topic = TopicPath.Of(caller);

        caller[1] = "__reserved";

        Assert.Equal("bind/rows", topic.ToKey());
        Assert.Equal("rows", topic.Segments[1]);
    }

    [Fact]
    public void EqualityIsTheJoinedNameBecauseThatIsWhatAProviderMatchesOn()
    {
        Assert.Equal(TopicPath.Of("a", "b"), TopicPath.Of("a", "b"));
        Assert.True(TopicPath.Of("a", "b") == TopicPath.Of("a", "b"));
        Assert.True(TopicPath.Of("a", "b") != TopicPath.Of("a", "c"));
        Assert.Equal(TopicPath.Of("a", "b").GetHashCode(), TopicPath.Of("a", "b").GetHashCode());

        // And {"a/b"} is not reachable at all, so the collision the join once had
        // cannot be constructed to compare against.
        Assert.Throws<ArgumentException>(() => TopicPath.Of("a/b"));
    }

    [Fact]
    public void TheUtf8IsEncodedOnceAndKept()
    {
        TopicPath topic = TopicPath.Of("bind", "rows");
        byte[][] first = topic.Utf8Segments;

        Assert.Same(first, topic.Utf8Segments);
        Assert.Equal(Encoding.UTF8.GetBytes("bind"), first[0]);
        Assert.Equal(Encoding.UTF8.GetBytes("rows"), first[1]);
    }
}

public class ProviderSelectorTests
{
    [Theory]
    [InlineData("inprocess")]
    [InlineData("fastdds")]
    [InlineData("xrce")]
    [InlineData("a-b_C9")]
    public void APlainWordIsAName(string text) => Assert.True(ProviderSelector.Parse(text).IsName);

    [Theory]
    [InlineData("x.so")]
    [InlineData("x.dll")]
    [InlineData("x.dylib")]
    [InlineData("./x")]
    [InlineData("/opt/lib/x.so")]
    [InlineData(@"C:\d\x.dll")]
    [InlineData(@"\\host\share\x.dll")]
    public void EverySpellingOfALibraryIsAPath(string text)
    {
        // The rule is total and disjoint, which is what lets ONE configuration
        // setting carry both kinds. Each of these carries a dot, a slash, a
        // backslash or a colon, so none of them is a name.
        Assert.False(ProviderSelector.Parse(text).IsName);
    }

    [Fact]
    public void TheOneMisclassificationIsDocumentedRatherThanFixed()
    {
        // A relative file with no dot and no separator classifies as a NAME. That
        // is the rule's one wrong answer and it is deliberate: it is loud, because
        // the name resolves against nothing and the refusal names what IS
        // available, and the operator's fix is "./myDriver". Pinned so that nobody
        // "corrects" the predicate into consulting the filesystem, which would make
        // one string mean different things in different working directories.
        Assert.True(ProviderSelector.Parse("myDriver").IsName);
        Assert.False(ProviderSelector.Parse("./myDriver").IsName);
    }

    [Fact]
    public void TheEmptyStringAndAnEmbeddedNulAreRefused()
    {
        Assert.Throws<ArgumentException>(() => ProviderSelector.Parse(""));

        // Not tidiness. "fastdds\0/../evil.so" classifies as a PATH and would
        // reach a future loader truncated, opening a different library with no
        // signal at all.
        ArgumentException refused = Assert.Throws<ArgumentException>(
            () => ProviderSelector.Parse("fastdds\0/../evil.so"));
        Assert.Contains("zero byte", refused.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ParsingNeverConsultsTheRegistrySoAStringMeansOneThingInEveryBuild()
    {
        // A name nothing has registered still PARSES. It fails later, at Create,
        // where the registry can say what is available - and that split is what
        // makes the classification independent of which providers a build linked.
        ProviderSelector unknown = ProviderSelector.Parse("nosuchprovider");

        Assert.True(unknown.IsName);
        Assert.Equal("nosuchprovider", unknown.Text);
    }

    [Fact]
    public void TheTextIsCarriedVerbatimWithNoTrimmingOrFolding()
    {
        Assert.Equal(" fastdds ", ProviderSelector.Parse(" fastdds ").Text);
        Assert.Equal("FastDDS", ProviderSelector.Parse("FastDDS").Text);

        // A space makes it a path, and case is not folded: "FastDDS" is a name,
        // and a different one from "fastdds".
        Assert.False(ProviderSelector.Parse(" fastdds ").IsName);
        Assert.True(ProviderSelector.Parse("FastDDS").IsName);
        Assert.NotEqual(ProviderSelector.Parse("FastDDS"), ProviderSelector.Parse("fastdds"));
    }

    [Fact]
    public void ADefaultedSelectorSaysSoRatherThanSelectingTheEmptyString()
    {
        ProviderSelector defaulted = default;

        Assert.Throws<InvalidOperationException>(() => defaulted.Text);
        Assert.Throws<InvalidOperationException>(() => defaulted.Utf8);
    }
}

public class PayloadBoundTests
{
    [Fact]
    public void TheConstantsAreTheSeamsOwnNumbers()
    {
        // ASSERTED AS LITERALS, from pubsub/include/fletcher/pubsub/payload_bound.hpp.
        // No entry point exports these, so this pair is checked by convention
        // rather than by construction - if you are here because this test failed,
        // that header is the other half to read.
        Assert.Equal(8u, PayloadBound.FramingBytes);
        Assert.Equal(4u, PayloadBound.Min);
        Assert.Equal((uint.MaxValue - 8u) & ~3u, PayloadBound.Max);
        Assert.Equal(4294967284u, PayloadBound.Max);
    }

    [Theory]
    [InlineData(0u, false)]     // UNSET, and false is what lets 0 mean that safely
    [InlineData(1u, false)]
    [InlineData(3u, false)]
    [InlineData(4u, true)]
    [InlineData(5u, false)]     // not 4-aligned
    [InlineData(1024u, true)]
    [InlineData(4294967284u, true)]
    [InlineData(4294967288u, false)]  // past the ceiling
    public void ABoundIsFourAlignedAndInsideTheCeiling(uint bytes, bool valid)
        => Assert.Equal(valid, PayloadBound.IsValid(bytes));

    [Fact]
    public void ZeroIsNotABoundWhichIsWhatLetsItMeanUnset()
    {
        // ProviderConfig spells "unset" as 0 and hands it to the provider. That is
        // only safe while no provider can mistake it for a real bound, which is
        // this one assertion.
        Assert.False(PayloadBound.IsValid(0));
    }
}
