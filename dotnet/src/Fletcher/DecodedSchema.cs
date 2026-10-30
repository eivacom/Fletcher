// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// What comes BACK is not always the schema that went out (D-BIND-39).
//
// ── The one case, and why it is not a special case ──────────────────────────
// A dictionary is a columnar optimisation: the indices mean nothing in a single
// row, so `docs/wire-format-specification.md` §"Dictionary Types" says the wire
// carries a dictionary field as its VALUE type, one value per row. The encoder
// resolves the index; the decoder therefore produces a plain value array.
//
// So a codec has two schemas — the one a caller BINDS against and the one DECODE
// produces — and they differ in exactly the dictionary fields. Importing a
// decoded array against the bind schema would tell `Apache.Arrow` to read a
// dictionary's index and value buffers from an array that has neither.
//
// ── Why this is derived here rather than fetched across the ABI ─────────────
// The rewrite is deterministic and it is specified, so the shim and every
// binding can each apply it and get the same answer. An ABI entry point for it
// would be a second source of truth that has to be kept in step, on a boundary
// whose whole design is to have as little in it as possible. The shim's own
// `NanoarrowCodec::decoded_schema()` is the same rewrite in C++, and
// `CodecTests` holds the two derivations to each other by decoding a real
// dictionary batch through the shim and importing it against this one.
using System.Collections.Generic;
using System.Linq;

using Apache.Arrow;
using Apache.Arrow.Types;

namespace Eiva.Fletcher;

/// <summary>Rewrites a schema the way the wire format decodes it.</summary>
internal static class DecodedSchema
{
    /// <summary>The schema <c>fl_decode_rows</c> produces for <paramref name="schema"/>.</summary>
    /// <remarks>
    /// Returns <paramref name="schema"/> ITSELF when nothing changes, which is
    /// every schema carrying no dictionary — the overwhelmingly common case. That
    /// is worth more than the allocation it saves: reference equality is what lets
    /// <see cref="FletcherCodec.DecodedSchema"/> be compared against
    /// <see cref="FletcherCodec.Schema"/> to ask "does this codec decode into
    /// something else?" without walking anything.
    /// </remarks>
    internal static Schema Resolve(Schema schema)
    {
        if (!schema.FieldsList.Any(ContainsDictionary))
        {
            return schema;
        }

        var builder = new Schema.Builder();
        foreach (Field field in schema.FieldsList)
        {
            builder.Field(ResolveField(field));
        }

        if (schema.Metadata is not null)
        {
            foreach (KeyValuePair<string, string> entry in schema.Metadata)
            {
                builder.Metadata(entry.Key, entry.Value);
            }
        }

        return builder.Build();
    }

    /// <summary>Does this field, or anything under it, carry a dictionary?</summary>
    private static bool ContainsDictionary(Field field) => ContainsDictionary(field.DataType);

    private static bool ContainsDictionary(IArrowType type) => type switch
    {
        DictionaryType => true,
        StructType structType => structType.Fields.Any(ContainsDictionary),
        ListType list => ContainsDictionary(list.ValueField),
        LargeListType largeList => ContainsDictionary(largeList.ValueField),
        FixedSizeListType fixedList => ContainsDictionary(fixedList.ValueField),

        // A map's entries are a struct of {key, value}; the spec allows a scalar
        // key only, but a VALUE may be composite and may therefore hide one.
        MapType map => ContainsDictionary(map.KeyField) || ContainsDictionary(map.ValueField),
        _ => false,
    };

    /// <summary>One field, with its identity kept and its type resolved.</summary>
    /// <remarks>
    /// The NAME, the NULLABILITY and the METADATA belong to the field, not to the
    /// dictionary's value type — which carries none of them. Dropping them here
    /// would produce a schema that decodes the right values under the wrong field
    /// names, which is the kind of wrong that passes a round-trip test.
    /// </remarks>
    private static Field ResolveField(Field field) =>
        new(field.Name, Resolve(field.DataType), field.IsNullable, field.Metadata);

    private static IArrowType Resolve(IArrowType type) => type switch
    {
        // The rule itself. Everything else in this switch exists only to reach it
        // through a composite.
        DictionaryType dictionary => dictionary.ValueType,

        StructType structType => new StructType(structType.Fields.Select(ResolveField).ToList()),
        ListType list => new ListType(ResolveField(list.ValueField)),
        LargeListType largeList => new LargeListType(ResolveField(largeList.ValueField)),
        FixedSizeListType fixedList =>
            new FixedSizeListType(ResolveField(fixedList.ValueField), fixedList.ListSize),
        MapType map => new MapType(ResolveField(map.KeyField), ResolveField(map.ValueField), map.KeySorted),
        _ => type,
    };
}
