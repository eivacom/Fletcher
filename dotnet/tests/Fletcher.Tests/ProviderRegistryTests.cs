// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-4b: selection, through the shim.
//
// These rows LOAD THE NATIVE SHIM, unlike ProviderVocabularyTests, which is pure
// managed validation. The split matters: the vocabulary tests pin what C# refuses
// before crossing, and these pin what happens when it does cross - including the
// two refusals the managed side deliberately does NOT reproduce, because the
// registry can say something a local check never could.
using System;
using System.Text;

using Eiva.Fletcher;

using Xunit;

namespace Eiva.Fletcher.Tests;

public class ProviderRegistryTests
{
    [Fact]
    public void TheBuiltInLoopbackIsSelectableByName()
    {
        using PubSubProviderHandle provider =
            ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());

        Assert.NotNull(provider);
    }

    [Fact]
    public void DisposingTwiceIsSafe()
    {
        // A finaliser in a foreign runtime has nowhere to put an exception, so
        // every release path on this surface has to tolerate being run twice.
        PubSubProviderHandle provider =
            ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());

        provider.Dispose();
        provider.Dispose();
    }

    [Fact]
    public void AnUnknownNameIsRefusedByTheRegistryAndTheMessageNamesWhatIsAvailable()
    {
        // THE REASON Parse does not consult a registry. A local check could only
        // have said "no"; the registry says no AND says what this build has, which
        // is the answer an operator with a typo actually needs.
        FletcherException refused = Assert.Throws<FletcherException>(
            () => ProviderRegistry.Create(ProviderSelector.Parse("nosuchprovider"), new ProviderConfig()));

        Assert.Contains("inprocess", refused.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void APathSelectorReachesTheResolverSeatAndIsRefusedThere()
    {
        // The classification is total, so a path is a legitimate selection - it
        // simply reaches a seat PDA-ABI has not filled yet. NotSupported rather
        // than InvalidArgument is the distinction: the string was understood, the
        // capability is absent.
        FletcherException refused = Assert.Throws<FletcherException>(
            () => ProviderRegistry.Create(ProviderSelector.Parse("./driver.so"), new ProviderConfig()));

        Assert.Equal(FletcherStatus.NotSupported, refused.Status);
    }

    [Fact]
    public void TheConfigurationIsRequired()
    {
        Assert.Throws<ArgumentNullException>(
            () => ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), null!));
    }

    [Fact]
    public void ADefaultedSelectorIsRefusedBeforeAnythingCrosses()
    {
        // Not a FletcherException: nothing reached the shim. A defaulted struct is
        // a managed mistake and gets a managed answer, with the caller's stack.
        Assert.Throws<InvalidOperationException>(
            () => ProviderRegistry.Create(default, new ProviderConfig()));
    }

    [Fact]
    public void TheDocumentCrossesByLengthAndTheProofIsWhereTheRefusalPointsTo()
    {
        // THE REFUSAL IS THE EVIDENCE, and it is stronger than a success would
        // have been. `inprocess` parses a key=value document and has no
        // representation for a NUL, so it refuses one - AND NAMES THE OFFSET.
        //
        // That offset is the whole test. If the shim marshalled the document as a
        // C string it would have stopped at the zero byte and handed the provider
        // "a=1", which is a perfectly valid document that parses and succeeds. The
        // only way a refusal naming offset 3 can happen is if all seven bytes
        // crossed - so the length is authoritative, end to end.
        //
        // This row was first written asserting that the create SUCCEEDED, which
        // passed for the wrong reason on a provider that ignored the tail and
        // would have passed identically against a truncating shim. The failure
        // corrected the test.
        byte[] document = Encoding.UTF8.GetBytes("a=1\0b=2");
        var config = new ProviderConfig { Document = document };

        FletcherException refused = Assert.Throws<FletcherException>(
            () => ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), config));

        Assert.Contains("offset 3", refused.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AnEmptyDocumentIsFine()
    {
        // The default: no document at all. Worth its own row because it is the path
        // where the pinned pointer is null, which is a different marshalling case
        // from a pinned non-empty array.
        using PubSubProviderHandle provider =
            ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());

        Assert.NotNull(provider);
    }

    [Fact]
    public void AnUnsetPayloadBoundIsAcceptedAndMeansTheProvidersOwnDefault()
    {
        // 0 is UNSET, not a refusal. Fletcher does not know any provider's valid
        // bounds, so it cannot demand a value it could check.
        var config = new ProviderConfig { MaxPayloadBytes = 0, DomainId = 0 };

        using PubSubProviderHandle provider =
            ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), config);

        Assert.NotNull(provider);
    }
}
