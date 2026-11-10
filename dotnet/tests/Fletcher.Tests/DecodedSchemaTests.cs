// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The dictionary rewrite, walked directly (D-BIND-39).
//
// ── Why this exists separately from the round-trip tests ────────────────────
// `DecodedSchema.Resolve` recurses through every composite the mapping produces,
// and the ruling that introduced it shipped with ONLY top-level dictionary
// fixtures — so each of those recursive branches was compiled and never
// executed. "Never checked" and "checked, found nothing" are the same observable,
// which is the shape BIND-2 was caught by twice.
//
// Reaching them through a round trip would mean building a dictionary inside a
// map value, a large list and a fixed-size list as real Arrow arrays, which is a
// lot of fixture for a property that is purely about SCHEMA. So the rewrite is
// tested where it lives: no native call, no arrays, one assertion per branch.
//
// The end-to-end half — that the SHIM's own recursion agrees with this one — is
// `ProtoMappingParityTests.ADictionaryInsideAStructRoundTripsAsItsValueType`,
// which encodes and decodes a nested dictionary for real. Both halves are needed:
// this one covers every branch, that one proves the two derivations match.
using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class DecodedSchemaTests
{
    private static DictionaryType Dictionary =>
        new(Int32Type.Default, StringType.Default, ordered: false);

    /// <summary>A schema with no dictionary is returned UNCHANGED, by identity.</summary>
    /// <remarks>
    /// Reference equality is the contract, not an optimisation detail: it is how a
    /// caller asks "does this codec decode into something else?" without walking
    /// anything, and <c>FletcherCodec.DecodedSchema</c> is documented on it.
    /// </remarks>
    [Fact]
    public void ASchemaWithoutDictionariesIsTheSameObject()
    {
        Schema schema = CodecFixtures.Composite().Schema;

        Assert.Same(schema, DecodedSchema.Resolve(schema));
    }

    /// <summary>A top-level dictionary becomes its value type, keeping its identity.</summary>
    [Fact]
    public void ATopLevelDictionaryBecomesItsValueType()
    {
        var schema = new Schema(
            [new Field("category", Dictionary, nullable: true)], metadata: null);

        Schema resolved = DecodedSchema.Resolve(schema);

        Assert.NotSame(schema, resolved);
        Assert.Equal("category", resolved.FieldsList[0].Name);
        Assert.Equal(ArrowTypeId.String, resolved.FieldsList[0].DataType.TypeId);
        Assert.True(resolved.FieldsList[0].IsNullable);
    }

    /// <summary>Inside a struct.</summary>
    [Fact]
    public void ADictionaryInsideAStructIsResolved()
    {
        var inner = new StructType(
        [
            new Field("id", Int32Type.Default, nullable: true),
            new Field("category", Dictionary, nullable: true),
        ]);

        var resolved = (StructType)Resolve(inner);

        Assert.Equal(ArrowTypeId.Int32, resolved.Fields[0].DataType.TypeId);
        Assert.Equal("category", resolved.Fields[1].Name);
        Assert.Equal(ArrowTypeId.String, resolved.Fields[1].DataType.TypeId);
    }

    /// <summary>Inside a list.</summary>
    [Fact]
    public void ADictionaryInsideAListIsResolved()
    {
        var resolved = (ListType)Resolve(new ListType(new Field("item", Dictionary, nullable: true)));

        Assert.Equal("item", resolved.ValueField.Name);
        Assert.Equal(ArrowTypeId.String, resolved.ValueDataType.TypeId);
    }

    /// <summary>Inside a large list.</summary>
    /// <remarks>
    /// `large_list` is not in the proto mapping — the shim's codec accepts it
    /// anyway, being a superset — so this branch would otherwise never be reached
    /// by any fixture in the tree.
    /// </remarks>
    [Fact]
    public void ADictionaryInsideALargeListIsResolved()
    {
        var resolved = (LargeListType)Resolve(
            new LargeListType(new Field("item", Dictionary, nullable: true)));

        Assert.Equal(ArrowTypeId.String, resolved.ValueDataType.TypeId);
    }

    /// <summary>Inside a fixed-size list, keeping the size.</summary>
    /// <remarks>
    /// The list size is the part a careless rewrite drops: it lives in the TYPE
    /// rather than on the wire, so losing it changes how many elements the decoder
    /// expects per value and nothing would say so until the bytes disagreed.
    /// </remarks>
    [Fact]
    public void ADictionaryInsideAFixedSizeListIsResolvedAndKeepsItsSize()
    {
        var resolved = (FixedSizeListType)Resolve(
            new FixedSizeListType(new Field("item", Dictionary, nullable: true), listSize: 3));

        Assert.Equal(ArrowTypeId.String, resolved.ValueDataType.TypeId);
        Assert.Equal(3, resolved.ListSize);
    }

    /// <summary>Inside a map's VALUE, keeping the key sortedness.</summary>
    /// <remarks>
    /// A map key must be a scalar, so a dictionary can only be the value — but the
    /// rewrite still has to descend through the entries struct to reach it, and
    /// `KeySorted` is carried on the type where a rebuild can silently lose it.
    /// </remarks>
    [Fact]
    public void ADictionaryInsideAMapValueIsResolved()
    {
        var resolved = (MapType)Resolve(
            new MapType(StringType.Default, Dictionary, keySorted: true));

        Assert.Equal(ArrowTypeId.String, resolved.KeyField.DataType.TypeId);
        Assert.Equal(ArrowTypeId.String, resolved.ValueField.DataType.TypeId);
        Assert.True(resolved.KeySorted);
    }

    /// <summary>Two levels down, because one level is not recursion.</summary>
    [Fact]
    public void ADictionaryTwoCompositesDeepIsResolved()
    {
        var inner = new StructType([new Field("category", Dictionary, nullable: true)]);
        var resolved = (ListType)Resolve(new ListType(new Field("item", inner, nullable: true)));

        var reached = (StructType)resolved.ValueDataType;
        Assert.Equal(ArrowTypeId.String, reached.Fields[0].DataType.TypeId);
    }

    /// <summary>Resolve one type by putting it through a one-field schema.</summary>
    private static IArrowType Resolve(IArrowType type)
    {
        var schema = new Schema([new Field("value", type, nullable: true)], metadata: null);
        return DecodedSchema.Resolve(schema).FieldsList[0].DataType;
    }
}
